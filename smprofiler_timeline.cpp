#include "smprofiler_timeline.h"

#include <utility>
#include <sstream>
#include <fstream>
#include <thread>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <regex>

bool TimelineWriter::open_file_and_init(std::string file_name){
  // Get directory path from file name.
  size_t slash = file_name.find_last_of(FORWARD_SLASH);
  std::string directory_path = file_name.substr(0, slash);

  // create directory tree.
  size_t pos = 0;
  while(pos != std::string::npos) {
    pos = directory_path.find(FORWARD_SLASH, pos + 1);
    mkdir(const_cast<char*>(directory_path.substr(0, pos).c_str()), FOLDER_PERMISSIONS);
  }
  // log warning if directory not created. Should we return false?
  struct stat buffer;
  if (stat (directory_path.c_str(), &buffer) != 0) {
    continuous_fail_count_++;
    return false;
  }

  file_.open(file_name, std::fstream::out | std::fstream::trunc);

  if (file_.is_open()) {
    // Initialize the timeline file with '[' character.
    file_ << "[\n";
    healthy_ = true;
    tensor_existed_ = false;
    tid_table_.clear();
    tensor_table_.clear();
    continuous_fail_count_ = 0;
    return true;
  } else {
    continuous_fail_count_++;
    return false;
  }
}

#define UTC (0)

std::string TimelineWriter::create_new_file_path(uint64_t timestamp_utc) {
  std::time_t tt = std::time(0);
  struct tm* ptm = gmtime(&tt);
  // Update the current hour as reflected in the folder name.
  cur_hour_ = ptm->tm_hour;

  // Generate timestamp for folder name.
  char date_time_format[] = "%Y%m%d%H";
  char time_str[] = "yyyymmddHHMMSSfff";
  strftime(time_str, strlen(time_str), date_time_format, ptm);

  // generate folder name
  std:: string timeline_folder_name = base_folder_ + FORWARD_SLASH + BASE_FOLDER_PATH_STR + time_str + FORWARD_SLASH;
  // create the base directory for the timeline file.
  size_t pos = 0;
  while(pos != std::string::npos) {
    pos = timeline_folder_name.find(FORWARD_SLASH, pos + 1);
    mkdir(const_cast<char*>(timeline_folder_name.substr(0, pos).c_str()), FOLDER_PERMISSIONS);
  }

  // path for the timeline file.
  cur_file_timestamp_ = timestamp_utc;
  std::string filepath_ = timeline_folder_name + std::to_string(timestamp_utc) + "_" + pid_node_id_ + "_model_timeline.json";
  return filepath_;
}

void TimelineWriter::Initialize(std::string node_id, uint64_t cur_time) {

  // read all the config parameters.
  base_folder_ = "/tmp";
  pid_node_id_ = "1";
  max_file_size_ = 100000000000;
  file_close_interval_ = 600000;
  continuous_fail_count_threshold_ = 4;
  tf_dataloader_start_flag_filepath = base_folder_ +  node_id + "/tf_dataloader_start_flag.tmp";
  tf_dataloader_end_flag_filepath = base_folder_ + node_id + "/tf_dataloader_end_flag.tmp";
  healthy_ = true;

  start_time_since_epoch_utc_micros_ = cur_time;
  // Initialize the last_file_close_time and last_event_end_time to start_time_
  // These values will get updated as the timeline gets written.
  last_file_close_time_ = start_time_since_epoch_utc_micros_;
  last_event_end_time_ = start_time_since_epoch_utc_micros_;

  // Get the hour for start time.
  time_t ts = last_file_close_time_;
  std::time_t now= std::time(0);
  std::tm* current_event_utc_tm = std::gmtime(&now);
  //struct tm * current_event_utc_tm = gmtime(&ts);
  cur_hour_ = current_event_utc_tm->tm_hour ;
  // This is the temporary current file that gets written to. While rotating the file we will close this file,
  // rename it to filename with appropriate timestamp, truncate this file and restart writing to it.
  current_tmp_filename_ = base_folder_ + "/framework/" + std::to_string(getpid()) + SMDEBUG_TEMP_PATH_SUFFIX;

  // Spawn writer thread.
  writer_thread = std::thread(&TimelineWriter::WriterLoop, this);
}

// Destructor for TimelineWriter which will ensure that file fstream object will be closed appropriately.
TimelineWriter::~TimelineWriter() {
  healthy_ = false;

  if(writer_thread.joinable()) {
    writer_thread.join();
  }
  if (file_ && file_.is_open()) {

    // Close the file resource.
    file_.flush();
    file_.close();

    //rename tmp file to appropriate filename with timestamp.
    std::rename(current_tmp_filename_.c_str(), create_new_file_path(last_event_end_time_).c_str());
  }
}

void TimelineWriter::EnqueueWriteEvent(const std::string& tensor_name,
                                       char phase, const std::string& op_name,
                                       const std::string& args,
                                       long rel_ts_micros, pthread_t threadid, pid_t pid, long duration) {
  TimelineRecord r;
  r.type = TimelineRecordType::EVENT;
  r.tensor_name = tensor_name;
  r.phase = phase;
  r.op_name = op_name;
  r.args = args;
  r.rel_ts_micros = rel_ts_micros;
  r.event_end_ts_micros_since_epoch_utc = start_time_since_epoch_utc_micros_ + rel_ts_micros + duration;
  r.duration = duration;
  r.threadid = threadid;
  r.pid = pid;
  std::lock_guard<std::recursive_mutex> guard(mutex_);
  record_queue_.push(r);
}

// this decides if existing open file should rotate.
bool TimelineWriter::shouldRotateToNew(uint64_t timestamp_micros_since_utc){
  // Get the hour info for the current event timestamp and compate for cur_hour.
  time_t tt = timestamp_micros_since_utc / MICROS_FACTOR;
  tm current_event_utc_tm = *gmtime(&tt);
  if (current_event_utc_tm.tm_hour != cur_hour_) {
    return true;
  }

  // check size of file.
  struct stat stat_buf;
  stat(current_tmp_filename_.c_str(), &stat_buf);
  if (stat_buf.st_size > max_file_size_) {
    return true;
  }

  // check file close interval
  if (((int64_t)(timestamp_micros_since_utc - last_file_close_time_) / MICROS_FACTOR) > file_close_interval_) {
    return true;
  }

  return false;
}

void TimelineWriter::close_and_rename_file() {
  struct timeval tv;
  gettimeofday(&tv,NULL);
  uint64_t cur_time = (1000000 * tv.tv_sec) + tv.tv_usec;
  file_.flush();
  file_.close();

  //rename tmp file to appropriate filename with timestamp.
  std::rename(current_tmp_filename_.c_str(), create_new_file_path(last_event_end_time_).c_str());
  last_file_close_time_ = cur_time;
}

void TimelineWriter::DoWriteEvent(const TimelineRecord& r) {
  // if existing file is > seconds old or size > MB , close this , create new file with new path interval
  // NOTE: need to save existing metadata strings in new file, so all strings for tensorIdx need to be saved in memory
  // This will make sure that all files are self readable , file closing and opening can be done at the end of function before we do file_seek
  // 1. 1.0 get date and hour from r.absolutetimestamp
      // 2.0 if shouldRotateToNew() : // checks if current file size has exceeded limit or event_ts is more than currnt hour, store_new hr to cur hr,
          //2.1 close existing file
          //2.2 file_name = create new file for event()//
          //2.3 if !open_file_and_init(file_name) : return
  //if (file_.is_open() && shouldRotateToNew(r.event_end_ts_micros_since_epoch_utc)) {
 //   close_and_rename_file();
 // }

  // If no file is open, create a new file, open and initialize it.
  if (!file_.is_open()) {

    if (!open_file_and_init(current_tmp_filename_)){
      // The number of continuous failures crossed the threshold.
      // Empty the queue and mark the writer unhealthy.
      if (continuous_fail_count_ > FILE_OPEN_FAIL_THRESHOLD) {
        healthy_ = false;
        record_queue_.empty();
      }
      return;
    }
  }

  // Note: Below this we expect that file_ is open pointing to right file where this event needs to be written

  if (r.event_end_ts_micros_since_epoch_utc > last_event_end_time_) {
    last_event_end_time_ = r.event_end_ts_micros_since_epoch_utc;
  }

  // if this file has tensors, then we need to go 2 characters back and overwrite with ,\n
  // Note that after every tensor write we make sure that file is valid json so we append
  // \n] , below we are overwriting '\n]' sentinal character written in file as we know that some tensor
  // was already written in the file (tensor_existed_ is True)
  if(tensor_existed_){
    // going to last \n which is 2 character behind from last
    long pos = file_.tellp();
    file_.seekp (pos-2);
    file_ << ",\n";
  }
  auto& tensor_idx = tensor_table_[r.tensor_name];
  if(tensor_idx == 0  || tid_table_.find(r.threadid) == tid_table_.end()){
    if(!tensor_existed_){
      file_ << "{";
      file_ << "\"name\": \"process_name\"";
      // Note name of process can be given in args{"name:"}
      file_ << ", \"ph\": \"M\"";
      file_ << ", \"pid\": " << 0 << "";
      file_ << ", \"args\": {\"start_time_since_epoch_in_micros\":" << start_time_since_epoch_utc_micros_ << "}";
      file_ << "}," << std::endl;
      file_ << "{";
      file_ << "\"name\": \"process_sort_index\"";
      file_ << ", \"ph\": \"M\"";
      file_ << ", \"pid\": " << 0 << "";
      file_ << ", \"args\": {\"sort_index\": " << 0 << "}";
      file_ << "}," << std::endl;
    }
    if(tensor_idx == 0){
      tensor_idx = (int)tensor_table_.size();
    // We model tensors as processes. Register metadata for this "pid".
      file_ << "{";
      file_ << "\"name\": \"process_name\"";
      file_ << ", \"ph\": \"M\"";
      file_ << ", \"pid\": " << tensor_idx << "";
      file_ << ", \"args\": {\"name\": \"" << r.tensor_name << "\"}";
      file_ << "}," << std::endl;
      file_ << "{";
      file_ << "\"name\": \"process_sort_index\"";
      file_ << ", \"ph\": \"M\"";
      file_ << ", \"pid\": " << tensor_idx << "";
      file_ << ", \"args\": {\"sort_index\": " << tensor_idx << "}";
      file_ << "}," << std::endl;
    }
    // thread id and sort thread index
    file_ << "{";
    file_ << "\"name\": \"thread_name\"";
    file_ << ", \"ph\": \"M\"";
    file_ << ", \"pid\": " << tensor_idx << "";
    file_ << ", \"tid\": " << r.threadid << "";

    file_ << ", \"args\": {\"name\":\"tid-" << r.threadid <<"_pid-"<< r.pid << "\"}";
    file_ << "}," << std::endl;
    file_ << "{";
    file_ << "\"name\": \"thread_sort_index\"";
    file_ << ", \"ph\": \"M\"";
    file_ << ", \"pid\": " << tensor_idx << "";
    file_ << ", \"tid\": " << r.threadid << "";
    file_ << ", \"args\": {\"sort_index\": " << r.threadid << "}";
    file_ << "}," << std::endl;

    tid_table_.insert(r.threadid);
  }
  file_ << "{";
  file_ << "\"ph\": \"" << r.phase << "\"";
  if (r.phase != 'E') {
    // Not necessary for ending event.
    file_ << ", \"name\": \"" << r.op_name << "\"";
  }
  file_ << ", \"ts\": " << r.rel_ts_micros << "";
  file_ << ", \"pid\": " << tensor_idx << "";
    file_ << ", \"tid\": " << r.threadid << "";

  if (r.phase == 'X') {
    file_ << ", \"dur\": " << r.duration << "";
  }
  if (r.args != "") {
    file_ << ", \"args\": {" << r.args << "}";
  }
  file_ << "}" << std::endl << "]";

  file_.flush();
  tensor_existed_ = true;
}

void TimelineWriter::WriterLoop() {
  while (healthy_) {
    update_dataloader_collection_status();
    struct timeval tv;
    gettimeofday(&tv,NULL);
    uint64_t cur_time = (1000000 * tv.tv_sec) + tv.tv_usec;
    if (file_.is_open() && shouldRotateToNew(cur_time)) {
      printf("rotate file\n");
      close_and_rename_file();
    }
    TimelineRecord r;
    {

      std::lock_guard<std::recursive_mutex> guard(mutex_);
      if(record_queue_.empty()){
        std::this_thread::yield();
        continue;
      }
      r = record_queue_.front();
      record_queue_.pop();
    }
    switch (r.type) {
      case TimelineRecordType::EVENT:
        DoWriteEvent(r);
        break;
      default:
        throw std::logic_error("Unknown event type provided.\n");
    }

    if (!file_.good()) {
      healthy_ = false;
      record_queue_.empty();
    }
    std::this_thread::yield();
  }
}

void TimelineWriter::update_dataloader_collection_status() {
  // Check the dataloader flags. If only the start flag is found, collect dataloader metrics.
  // Otherwise, don't collect dataloader metrics.

  if(!file_exists(tf_dataloader_start_flag_filepath)) {
    should_collect_dataloader_metrics_ = false;
    return;
  } else if (file_exists(tf_dataloader_end_flag_filepath)) {
    should_collect_dataloader_metrics_ = false;
    return;
  }

  should_collect_dataloader_metrics_ = true;
}

bool TimelineWriter::file_exists(std::string filename) {
  struct stat buffer;
  return (stat (filename.c_str(), &buffer) == 0);
}

void Timeline::Initialize() {
  if (initialized_) {
    return;
  }
  struct timeval tv;
  gettimeofday(&tv,NULL);
  start_time_ = (1000000 * tv.tv_sec) + tv.tv_usec;

  // create the config reader instance.
  node_id = "algo-1";

  // Start the writer.
  writer_->Initialize(node_id, start_time_);

  // Initialize if we were able to open the file successfully.
  initialized_ = writer_->IsHealthy();
}

// record complete envents
// training phase can be strings like, data_iterating, forward, backward, operations etc
// op_name can be more details about phase like whether dataset or iterator
// args can be process id and thread id
// long long ts_micros is start_time for duration event
// phse for this is defaulted to 'X'
void Timeline::SMRecordEvent(const std::string training_phase,
                          const std::string op_name, uint64_t start_ts, uint64_t duration, const std::string args, char event_type){//, uint64_t tid) {
  if (!initialized_ || !writer_->IsHealthy()) {
    // Timeline Writer is an unhealthy state. Dropping the current event.
//    return;
  }

  if (!writer_->ShouldCollectDataloaderMetrics()) {
//    return;
  }

  //uint64_t now = EnvTime::NowMicros();
  //struct timeval tv;
  //gettimeofday(&tv,NULL);
  //uint64_t now = (1000000 * tv.tv_sec) + tv.tv_usec;

  //auto duration = now - start_ts;
  // relative time from start of the process.
  //auto rel_ts_micros = start_ts - start_time_;
  // get pid of writing process and tid
  pid_t pid = getpid();
  pthread_t threadid = pthread_self();
  std::string ss;
  ss.append("\"pid\":");
  ss.append(std::to_string(getpid()));
  ss.append(", \"thread_id\":");
  ss.append(std::to_string(threadid));

  if(args.size() > 1)
        ss.append(args);
  //writer_->EnqueueWriteEvent(training_phase, event_type, op_name, ss,
  //                           rel_ts_micros, threadid, pid, duration);
  writer_->EnqueueWriteEvent(training_phase, event_type, op_name, ss, start_ts-start_time_, threadid, pid, duration);
}

Timeline&  Timeline::getInstance() {
    static Timeline instance(std::move([]()->Timeline{
      return Timeline();
      }()));
    return instance;
}

Timeline::Timeline(){
  writer_ = std::make_unique<TimelineWriter>();
  Initialize();
}

static bool __smdebug_profiler_initialized__1234_ = []()->bool{
  Timeline::getInstance();
  return true;
  }();
