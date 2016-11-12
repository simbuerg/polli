#include "polli/Db.h"
#include "polli/log.h"
#include <pqxx/pqxx>

#include <ctime>
#include <set>
#include <stdlib.h>
#include <string>
#include <thread>
#include <numeric>
#include <iostream>

using namespace pqxx;

namespace polli {
struct DbOptions {
  std::string host;
  int port;
  std::string user;
  std::string pass;
  std::string name;
  uint64_t run_id;
  std::string uuid;
  std::string exp_uuid;
};

static DbOptions getDBOptionsFromEnv() {
  DbOptions Opts;

  const char *host = std::getenv("BB_DB_HOST");
  const char *user = std::getenv("BB_DB_USER");
  const char *pass = std::getenv("BB_DB_PASS");
  const char *name = std::getenv("BB_DB_NAME");
  const char *port = std::getenv("BB_DB_PORT");
  const char *run_id = std::getenv("BB_DB_RUN_ID");
  const char *uuid = std::getenv("BB_DB_RUN_GROUP");
  const char *exp_uuid = std::getenv("BB_EXPERIMENT_ID");

  Opts.host = host ? host : "localhost";
  Opts.port = port ? std::stoi(port) : 5432;
  Opts.name = name ? name : "pprof";
  Opts.user = user ? user : "pprof";
  Opts.pass = pass ? pass : "pprof";
  Opts.run_id = run_id ? std::stoi(run_id) : 0;
  Opts.uuid = uuid ? uuid : "00000000-0000-0000-0000-000000000000";
  Opts.exp_uuid = exp_uuid ? exp_uuid : "00000000-0000-0000-0000-000000000000";

  return Opts;
}

std::string now() {
  char buf[sizeof "YYYY-MM-DDTHH:MM:SS"];
  time_t now;
  time(&now);

  strftime(buf, sizeof buf, "%F %T", localtime(&now));
  return std::string(buf);
}

class DBConnection {
  std::unique_ptr<pqxx::connection> c;

  void connect() {
    std::string CONNECTION_FMT_STR =
      "user={} port={} host={} dbname={} password={}";
    DbOptions Opts = getDBOptionsFromEnv();
    std::string connection_str =
        fmt::format(CONNECTION_FMT_STR, Opts.user, Opts.port, Opts.host,
                    Opts.name, Opts.pass);

    c = std::unique_ptr<pqxx::connection>(new pqxx::connection(connection_str));
    if (c) {
      std::string SELECT_RUN =
          "SELECT id,type,timestamp FROM papi_results WHERE run_id=$1 ORDER BY "
          "timestamp;";
      std::string SELECT_SIMPLE_RUN =
          "SELECT id,type,start,duration,name,tid FROM benchbuild_events WHERE run_id=$1 ORDER BY "
          "start;";
      std::string DELETE_SIMPLE_RUN =
          "DELETE FROM benchbuild_events WHERE run_id=$1";
      std::string SELECT_RUN_IDs =
          "SELECT id FROM run WHERE run_group = $1;";
      std::string SELECT_RUN_GROUPS =
          "SELECT DISTINCT run_group FROM run WHERE experiment_group = $1;";

      c->prepare("select_run", SELECT_RUN);
      c->prepare("select_simple_run", SELECT_SIMPLE_RUN);
      c->prepare("delete_simple_run", DELETE_SIMPLE_RUN);
      c->prepare("select_run_ids", SELECT_RUN_IDs);
      c->prepare("select_run_groups", SELECT_RUN_GROUPS);
    }
  }

public:
  DBConnection() { connect(); }

  pqxx::connection &operator->() {
    if (c)
      return *c;
    connect();
    return *c;
  }

  pqxx::connection &operator*() {
    if (c)
      return *c;
    connect();
    return *c;
  }

  ~DBConnection() {
    c->disconnect();
    c.reset(nullptr);
  }
};

static DBConnection& getDatabase() {
  static DBConnection DB;
  return DB;
}

static pqxx::result submit(const std::string &Query,
                           pqxx::work &w) throw(pqxx::syntax_error) {
  pqxx::result res;
  try {
    res = w.exec(Query);
  } catch (pqxx::data_exception e) {
    std::cerr << "pgsql: Encountered the following error:\n";
    std::cerr << e.what();
    std::cerr << "\n";
    std::cerr << e.query();
    throw e;
  }
  return res;
}

static Options getOptions() {
  Options Opts;

  const char *exp = std::getenv("BB_EXPERIMENT");
  const char *prj = std::getenv("BB_PROJECT");
  const char *dom = std::getenv("BB_DOMAIN");
  const char *grp = std::getenv("BB_GROUP");
  const char *uri = std::getenv("BB_SRC_URI");
  const char *cmd = std::getenv("BB_CMD");
  const char *db = std::getenv("BB_USE_DATABASE");
  const char *csv = std::getenv("BB_USE_CSV");
  const char *file = std::getenv("BB_USE_FILE");
  const char *exec = std::getenv("BB_ENABLE");

  Opts.experiment = exp ? exp : "unknown";
  Opts.project = prj ? prj : "unknown";
  Opts.domain = dom ? dom : "unknown";
  Opts.group = grp ? grp : "unknown";
  Opts.src_uri = uri ? uri : "unknown";
  Opts.command = cmd ? cmd : "unknown";
  Opts.use_db = db ? (bool)std::stoi(db) : true;
  Opts.use_csv = csv ? (bool)std::stoi(csv) : false;
  Opts.use_file = file ? (bool)std::stoi(file) : false;
  Opts.execute_atexit = exec ? (bool)std::stoi(exec) : true;

  return Opts;
}

struct Event {
  std::string Name;
  uint64_t ID;
  uint64_t Time;
};

void StoreRun(const EventMapTy &Events, const RegionMapTy &Regions) {
  Options opts = getOptions();
  DbOptions Opts = getDBOptionsFromEnv();

  std::string SEARCH_PROJECT_SQL =
      "SELECT name FROM project WHERE name = '{}';";

  std::string NEW_PROJECT_SQL =
      "INSERT INTO project (name, description, src_url, domain, group_name) "
      "VALUES ('{}', '{}', '{}', '{}', '{}');";

  std::string NEW_RUN_SQL =
      "INSERT INTO run (\"end\", command, "
      "project_name, experiment_name, run_group, experiment_group) "
      "VALUES (TIMESTAMP '{}', '{}', "
      "'{}', '{}', '{}', '{}') RETURNING id;";
  std::string NEW_RUN_RESULT_SQL = "INSERT INTO regions (name, id, "
                                   "duration, run_id) "
                                   "VALUES";

  pqxx::work w(*getDatabase());
  pqxx::result project_exists =
      submit(fmt::format(SEARCH_PROJECT_SQL, opts.project), w);

  if (project_exists.affected_rows() == 0)
    submit(fmt::format(NEW_PROJECT_SQL, opts.project, opts.project,
                       opts.src_uri, opts.domain, opts.group),
           w);

  uint64_t run_id = 0;
  if (!Opts.run_id) {
    pqxx::result r =
        submit(fmt::format(NEW_RUN_SQL, now(), opts.command, opts.project,
                           opts.experiment, Opts.uuid, Opts.exp_uuid),
               w);
    r[0]["id"].to(run_id);
  } else {
    run_id = Opts.run_id;
  }

  int cnt = 0;
  std::stringstream vals;
  for (auto KV : Events) {
    if (cnt > 0)
      vals << ",";
    vals << fmt::format(" ('{:s}', {:d}, {:d}, {:d})",
                        Regions.at(KV.first), KV.first, KV.second, run_id);
    cnt++;
  }
  vals << ";";
  submit(NEW_RUN_RESULT_SQL + vals.str(), w);
  vals.clear();
  vals.flush();
  w.commit();
}
}