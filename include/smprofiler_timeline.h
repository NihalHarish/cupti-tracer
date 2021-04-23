#include <atomic>
#include <fstream>
#include <unistd.h>
#include <iostream>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <set>
#include <vector>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <sys/time.h>

enum TimelineRecordType { EVENT, MARKER };

struct TimelineRecord {
  TimelineRecordType type;
  std::string tensor_name;
  char phase;
  std::string op_name;
  std::string args;
  std::string marker_name;
  long rel_ts_micros;
  long event_end_ts_micros_since_epoch_utc;
  long duration;
  pthread_t threadid;
  pid_t pid;
};

class TimelineWriter {
public:
  void Initialize(std::string node_id, uint64_t cur_time);
  inline bool IsHealthy() const { return healthy_; }
  inline bool ShouldCollectDataloaderMetrics() const { return should_collect_dataloader_metrics_; }
  void EnqueueWriteEvent(const std::string& tensor_name, char phase,
                         const std::string& op_name, const std::string& args,
                         long ts_micros, pthread_t threadid, pid_t pid, long duration=0);
  ~TimelineWriter();
  uint64_t start_time_since_epoch_utc_micros_;
  

private:
  void DoWriteEvent(const TimelineRecord& r);
  void WriterLoop();
  bool open_file_and_init(std::string file_name);
  bool shouldRotateToNew(uint64_t absolute_event_ts);
  std::string create_new_file_path(uint64_t timestamp_utc);
  void close_and_rename_file();
  void update_dataloader_collection_status();
  bool file_exists(std::string filename);

  std::thread writer_thread;
  int64_t max_file_size_;
  std::string base_folder_ = "";
  std::string node_id_ = "";
  std::string pid_node_id_ = "";
  std::string tf_dataloader_start_flag_filepath;
  std::string tf_dataloader_end_flag_filepath;
  int64_t file_close_interval_;
  int64_t continuous_fail_count_threshold_;
  int continuous_fail_count_ = 0;
  long cur_file_timestamp_;
  uint64_t last_event_end_time_;

  // Are we healthy?
  std::atomic_bool healthy_{false};
  std::atomic_bool should_collect_dataloader_metrics_{false};

  // Timeline file.
  std::fstream file_;
  // tensor_Existed_ is to find if this is first write in the file
  // see smprofiler_timeline.cc::DoWriteEvent 
  bool tensor_existed_ = false;
  uint64_t last_file_close_time_;
  int cur_hour_;
  std::string current_tmp_filename_;
  // Timeline record queue.
  std::queue<TimelineRecord> record_queue_;
  std::map<std::string, int> tensor_table_;
  // putting tid in table.
  std::set<pthread_t> tid_table_;
  // A mutex that guards timeline state from concurrent access.
  std::recursive_mutex mutex_;

  const std::string BASE_FOLDER_PATH_STR = "framework/pevents/";
  const int FOLDER_PERMISSIONS = 0755;
  const int MICROS_FACTOR = 1000000;
  const int FILE_OPEN_FAIL_THRESHOLD = 50;
  const std::string SMDEBUG_TEMP_PATH_SUFFIX = ".tmp";
  const std::string FORWARD_SLASH = "/";

};

enum TimelineState { UNKNOWN, NEGOTIATING, TOP_LEVEL, ACTIVITY };

// Writes timeline in Chrome Tracing format. Timeline spec is from:
// https://github.com/catapult-project/catapult/tree/master/tracing
class Timeline {
public:
  
  static Timeline& getInstance();
  Timeline(Timeline const&) = delete;
  void operator=(Timeline const&)  = delete;
  void Initialize();
  inline bool Initialized() const { return initialized_; }
  void SMRecordEvent(const std::string training_phase,
                          const std::string op_name, uint64_t start_ts, uint64_t duration, const std::string args = "", char event_type='X');
  uint64_t start_time_;

private:
  
  Timeline();
  Timeline(Timeline&&) = default;
  // Boolean flag indicating whether Timeline was initialized (and thus should
  // be recorded).
  bool initialized_ = false;
  // Data Loader Config parameters.
  std::string base_folder;
  std::string node_id;

  // Timeline writer.
  std::unique_ptr<TimelineWriter> writer_;
};

