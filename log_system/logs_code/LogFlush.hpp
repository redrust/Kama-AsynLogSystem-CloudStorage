#include <cassert>
#include <fstream>
#include <memory>
#include <unistd.h>
#include <filesystem>
#include "Util.hpp"

extern mylog::Util::JsonData* g_conf_data;
namespace mylog{
    class LogFlush
    {
    public:
        using ptr = std::shared_ptr<LogFlush>;
        virtual ~LogFlush() {}
        virtual void Flush(const char *data, size_t len) = 0;//不同的写文件方式Flush的实现不同
    };

    class StdoutFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<StdoutFlush>;
        void Flush(const char *data, size_t len) override{
            cout.write(data, len);
        }
    };
    class FileFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<FileFlush>;
        FileFlush(const std::string &filename) : filename_(filename)
        {
            // 创建所给目录
            Util::File::CreateDirectory(Util::File::Path(filename));
            // 打开文件
            fs_ = fopen(filename.c_str(), "ab");
            if(fs_==NULL){
                std::cout <<__FILE__<<__LINE__<<"open log file failed"<< std::endl;
                perror(NULL);
            }
        }
        void Flush(const char *data, size_t len) override{
            fwrite(data,1,len,fs_);
            if(ferror(fs_)){
                std::cout <<__FILE__<<__LINE__<<"write log file failed"<< std::endl;
                perror(NULL);
            }
            if(g_conf_data->flush_log == 1){
                if(fflush(fs_)==EOF){
                    std::cout <<__FILE__<<__LINE__<<"fflush file failed"<< std::endl;
                    perror(NULL);
                }
            }else if(g_conf_data->flush_log == 2){
                fflush(fs_);
                fsync(fileno(fs_));
            }
        }

    private:
        std::string filename_;
        FILE* fs_ = NULL; 
    };

    class RollFileFlush : public LogFlush
    {
    public:
        using ptr = std::shared_ptr<RollFileFlush>;
        RollFileFlush(const std::string &filename, size_t max_size)
            : max_size_(max_size), basename_(filename)
        {
            log_file_path_ = Util::File::Path(filename);
            Util::File::CreateDirectory(log_file_path_);
        }

        void Flush(const char *data, size_t len) override
        {
            // 确认文件大小不满足滚动需求
            InitLogFile();
            // 向文件写入内容
            fwrite(data, 1, len, fs_);
            if(ferror(fs_)){
                std::cout <<__FILE__<<__LINE__<<"write log file failed"<< std::endl;
                perror(NULL);
            }
            cur_size_ += len;
            if(g_conf_data->flush_log == 1){
                if(fflush(fs_)){
                    std::cout <<__FILE__<<__LINE__<<"fflush file failed"<< std::endl;
                    perror(NULL);
                }
            }else if(g_conf_data->flush_log == 2){
                fflush(fs_);
                fsync(fileno(fs_));
            }

            CheckLogFile();
        }

    private:
        void InitLogFile()
        {
            if(g_conf_data->log_file_mode == Util::JsonData::LogFileMode::SIZE_ROLL)
            {
                if (fs_==NULL || cur_size_ >= max_size_)
                {
                    reopenFile();
                    cur_size_ = 0;
                }
            }
            else if (g_conf_data->log_file_mode == Util::JsonData::LogFileMode::TIME_ROLL)
            {
                time_t now = Util::Date::Now();
                if (fs_ == NULL || now - last_roll_file_ts_ >= g_conf_data->rolling_interval)
                {
                    reopenFile();
                    last_roll_file_ts_ = now;
                }
            }
        }

        void CheckLogFile()
        {
            std::filesystem::file_time_type ft = std::filesystem::file_time_type::clock::now();
            uint64_t now = GetFileLastWriteTimeSec(ft);
            size_t rentention_sec = g_conf_data->retention_days * 24 * 3600;
            if (now - last_check_log_file_ts_ < rentention_sec){
                return;
            }
            last_check_log_file_ts_ = now;
            auto files = GetFilesInDirectory();
            if(files.size() <= 1 )
            {
                return;
            }
            std::sort(files.begin(), files.end(),
                      [](const std::filesystem::directory_entry &a,
                         const std::filesystem::directory_entry &b) {
                          return a.last_write_time() < b.last_write_time();
                      });
            // 遍历所有日志文件
            for(auto it = files.begin(); it != files.end(); ++it)
            {
                // 删除超过保留天数的日志文件
                auto last_write_sec = std::chrono::duration_cast<std::chrono::seconds>(ft - it->last_write_time()).count();
                if (last_write_sec > rentention_sec)
                {
                    std::filesystem::remove(it->path());
                }
            }
        }

        // 构建落地的滚动日志文件名称
        std::string CreateFilename()
        {
            time_t time_ = Util::Date::Now();
            struct tm t;
            localtime_r(&time_, &t);
            std::string filename = basename_;
            filename += std::to_string(t.tm_year + 1900);
            filename += std::to_string(t.tm_mon + 1);
            filename += std::to_string(t.tm_mday);
            filename += std::to_string(t.tm_hour + 1);
            filename += std::to_string(t.tm_min + 1);
            filename += std::to_string(t.tm_sec + 1) + '-' +
                        std::to_string(cnt_++) + ".log";
            return filename;
        }

    private:

        std::vector<std::filesystem::directory_entry> GetFilesInDirectory()
        {
            std::vector<std::filesystem::directory_entry> files;
            for (const auto &entry : std::filesystem::directory_iterator(log_file_path_))
            {
                if (entry.is_regular_file())
                {
                    files.push_back(entry);
                }
            }
            return files;
        }

        uint64_t GetFileLastWriteTimeSec(const std::filesystem::file_time_type &ft)
        {
            auto fs_now = std::filesystem::file_time_type::clock::now();
            return std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count();
        }

        void reopenFile()
        {
            if(fs_!=NULL){
                fclose(fs_);
                fs_=NULL;
            }   
            std::string filename = CreateFilename();
            fs_=fopen(filename.c_str(), "ab");
            if(fs_==NULL){
                std::cout <<__FILE__<<__LINE__<<"open file failed"<< std::endl;
                perror(NULL);
            }
        }

        size_t cnt_ = 1;
        size_t cur_size_ = 0;
        size_t max_size_;
        std::string basename_;
        // std::ofstream ofs_;
        FILE* fs_ = NULL;
        time_t last_check_log_file_ts_ = 0;
        std::string log_file_path_;
        time_t last_roll_file_ts_ = 0; // 上次滚动日志的时间戳
    };

    class LogFlushFactory
    {
    public:
        using ptr = std::shared_ptr<LogFlushFactory>;
        template <typename FlushType, typename... Args>
        static std::shared_ptr<LogFlush> CreateLog(Args &&...args)
        {
            return std::make_shared<FlushType>(std::forward<Args>(args)...);
        }
    };
} // namespace mylog