/* Copyright 2015- Benyu Zhang */

#define _Bool bool
#include <glog/logging.h>
#include <google/gflags.h>
#include <nxweb/nxweb.h>

#ifdef __cplusplus
extern "C"
{
#endif
extern void handler_config_run();
#ifdef __cplusplus
}
#endif

DEFINE_bool(daemon, false, "run as daemon");
DEFINE_bool(shutdown, false, "shutdown daemon via pid");
DEFINE_string(work_dir, "./", "work_dir, the static file is served from work_dir/www/root");
DEFINE_string(error_log_level, "NONE", "");
DEFINE_string(error_log_file, "", "default error_log in daemon");
DEFINE_string(access_log_file, "", "in apache format");
DEFINE_string(pid_file, "", "default pid in daemon mode");
DEFINE_string(user_name, "", "");
DEFINE_string(group_name, "", "");
DEFINE_string(host_and_port, "0.0.0.0:8080", "it can be :8080");

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);
  LOG(ERROR) << google::GetArgv();

  if (FLAGS_shutdown) {
    if (FLAGS_pid_file.empty()) {
      FLAGS_pid_file = "pid";
    }
    nxweb_shutdown_daemon(FLAGS_work_dir.c_str(), FLAGS_pid_file.c_str());
    return EXIT_SUCCESS;
  }

  nxweb_server_config.access_log_fpath=(FLAGS_access_log_file.empty()? nullptr: FLAGS_access_log_file.c_str());

  nxweb_main_args.user_uid=nxweb_get_uid_by_name(FLAGS_user_name.c_str());
  nxweb_main_args.group_gid=nxweb_get_gid_by_name(FLAGS_group_name.c_str());

  if (FLAGS_error_log_level == "INFO") nxweb_error_log_level=NXWEB_LOG_INFO;
  else if (FLAGS_error_log_level == "WARN") nxweb_error_log_level=NXWEB_LOG_WARNING;
  else if (FLAGS_error_log_level == "ERROR") nxweb_error_log_level=NXWEB_LOG_ERROR;
  else nxweb_error_log_level=NXWEB_LOG_NONE;

  nxweb_main_args.http_listening_host_and_port=FLAGS_host_and_port.c_str();

  if (FLAGS_daemon) {
    nxweb_server_config.error_log_fpath=(FLAGS_error_log_file.empty()? "error_log": FLAGS_error_log_file.c_str());
    nxweb_run_daemon(FLAGS_work_dir.c_str(),
                     nxweb_server_config.error_log_fpath,
                     FLAGS_pid_file.empty()? "pid": FLAGS_pid_file.c_str(),
                     handler_config_run, nxweb_main_args.group_gid, nxweb_main_args.user_uid);
  } else {
    nxweb_server_config.error_log_fpath=(FLAGS_error_log_file.empty()? nullptr: FLAGS_error_log_file.c_str());
    nxweb_run_normal(FLAGS_work_dir.c_str(),
                     nxweb_server_config.error_log_fpath,
                     FLAGS_pid_file.empty()? nullptr: FLAGS_pid_file.c_str(),
                     handler_config_run, nxweb_main_args.group_gid, nxweb_main_args.user_uid);
  }
  return EXIT_SUCCESS;
}
