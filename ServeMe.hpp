///// REST API SERVICE /////
// version 1.0
// author: Egor Fortov, 9.03.2024, @Copyright
// Brief documentation of the current REST API
// Capabilities:
// 1. GET/POST requests handling
// 2. Response Raw data + data from file
// 3. Exceptions handling (signals - not yet)
// 4. Easy-to-use public API (see RESTAPIAPP class)
// 5. Basic templates
// 6. Availability to inherit & fast create custom servers
// 7. Logging info with levels and modes
// 8. Logging into file & syslog
// 9. Caching queries
// 10. Easy connect: just include this .hpp file into your project
// Dependency libraries: boost lib
// Dependency includes: see below (8 includes)
// Feature: Hard parallelism under the hood
// For more read inline comments & official documentation of boost library
// Updates are comming...
////////////////////////////

#include <boost/asio.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <syslog.h>
#include <vector>

namespace Utils {
//#define DEBUG  // uncomment this line to see all Logs (this macros enables debug logs)
    namespace {
        std::mutex mu;
        std::string filePrefix = "@file:";
        typedef std::unordered_map<std::string, std::string> CACHE;

        enum class Level {
            Debug = 0,
            Info,
            Warning,
            Error,
            Critical
        };

        enum class Method {
            GET = 0,
            POST
        };

        int getPriority(Level level) noexcept {
            switch (level) {
                case Level::Debug:
                    return LOG_DEBUG;
                case Level::Info:
                    return LOG_INFO;
                case Level::Warning:
                    return LOG_WARNING;
                case Level::Error:
                    return LOG_ERR;
                case Level::Critical:
                    return LOG_CRIT;
                default:
                    return LOG_INFO;
            }
        }

        std::string getPrefix(Level level) noexcept {
            switch (level) {
                case Level::Debug:
                    return "[DEBUG]";
                case Level::Info:
                    return "[INFO]";
                case Level::Warning:
                    return "[WARNING]";
                case Level::Error:
                    return "[ERROR]";
                case Level::Critical:
                    return "[CRITICAL]";
                default:
                    return "[INFO]";
            }
        }
    }// namespace

    namespace Interfaces {
        class HttpServerInterface {
        public:
            virtual void addEndpoint(const std::string &path, const std::string &response, Method method) = 0;
        };
        class LoggerInterface {
        public:
            virtual void log(Level level, const std::string &message) noexcept = 0;
        };
        class HttpSessionInterface {
        public:
            virtual void start() = 0;
        };
        class RESTAPIAPPInterface {
        public:
            virtual void AddEndpoint(const std::string &path, const std::string &response, const std::string &method) = 0;
            virtual void RunServer() noexcept = 0;
            virtual void StopServer() noexcept = 0;
        };
    }// namespace Interfaces


    class Logger : Interfaces::LoggerInterface {
    public:
        Logger(const std::string &program_name = "HTTPServer", const std::string &log_file_name = "log.txt",
               bool syslog_enabled = true) try : syslogEnabled(syslog_enabled) {
            try {
                if (syslog_enabled) {
                    openlog(program_name.c_str(), LOG_CONS | LOG_NDELAY | LOG_PID, LOG_USER);
                }
                logFile.open(log_file_name, std::ios::out | std::ios::app);
#ifdef DEBUG
                std::cout << getPrefix(Level::Debug) << " Logger object created";
#endif
            } catch (...) {
                std::cerr << getPrefix(Level::Critical) + " Failed to open log file and/or system log\n";
            }
        } catch (...) {
            std::cerr << "Failed to create Logger object\n";
        }

        ~Logger() {
            try {
                closelog();
                logFile.close();
#ifdef DEBUG
                std::cout << getPrefix(Level::Debug) << " HttpSession object destroyed\n";
#endif
            } catch (...) {
                std::cerr << getPrefix(Level::Critical) + " Failed to close log file and/or system log\n";
            }
        }

        /// public API
        /// @param level - the type of the logging, see enum Level
        /// @param message - the log message, std::string
        void log(Level level, const std::string &message) noexcept override {
            if (syslogEnabled) {
                try {
                    writeToSyslog(level, message);
                } catch (...) {
                    std::cerr << getPrefix(Level::Error) + " Failed to log to system log\n";
                }
            }
            try {
                writeToFile(level, message);
            } catch (...) {
                std::cerr << getPrefix(Level::Error) + " Failed to log to file\n";
            }
        }

        typedef std::shared_ptr<Logger> Ptr;

    private:
        void writeToSyslog(Level level, const std::string &message) {  // @TODO later: asynchronous write
            int priority = getPriority(level);
            char buffer[80] = {0};
            std::time_t result = std::time(nullptr);
            std::strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", std::localtime(&result));
            std::lock_guard lock(mu);
            syslog(priority, "%s", (std::string(buffer) + message).c_str());  // @TODO: check workability
        }

        void writeToFile(Level level, const std::string &message) {  // @TODO later: asynchronous write
            std::string prefix = std::move(getPrefix(level));
            char buffer[80] = {0};
            std::time_t result = std::time(nullptr);
            std::strftime(buffer, 80, "%Y-%m-%d %H:%M:%S", std::localtime(&result));
            std::lock_guard lock(mu);
            logFile << buffer << " " << prefix << " " << message << std::endl;
        }

        std::ofstream logFile;
        const bool syslogEnabled;
    };

    namespace Templates::Responses {
        const auto OK = [](const std::string &body = "Hello, World!", const std::string &content_type = "text/html") {
            return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.length()) + "\r\nContent-Type: " + content_type + "\r\n\r\n" + body;
        };
        const auto NOT_OK = [](const std::string &body = "404 Not Found!") {
            return "HTTP/1.1 404 Not Found\r\nContent-Length: 14\r\n\r\n" + body;
        };
    }// namespace Templates::Responses

    namespace {
        typedef std::unordered_map<std::string, std::pair<std::string, Method>> endpoints;

        std::string readFileIntoString(const std::string &filename, Logger::Ptr logger) {
            std::ifstream file(filename);
            if (!file.is_open()) {
                logger->log(Level::Error, "Can not open file " + filename);
                return "";
            } else {
#ifdef DEBUG
                logger->log(Level::Debug, "file " + filename + " opened successfully");
#endif
            }

            std::string content((std::istreambuf_iterator<char>(file)), (std::istreambuf_iterator<char>()));
            return content;
        }

        std::string getBody(const std::string& str, Logger::Ptr logger) {
//            std::string currentPath = std::filesystem::current_path().c_str();  // for easy detect where you are
//            std::cout << "Current file path: " << currentPath << std::endl;  // for easy detect where you are
            if (str.substr(0, filePrefix.size()) == filePrefix) {
                std::string ans = readFileIntoString(str.substr(filePrefix.size()), logger);
                return ans;
            } else {
                return str;
            }
        }
    }// namespace

    class HttpSession : public std::enable_shared_from_this<HttpSession>, Interfaces::HttpSessionInterface {
    public:
        HttpSession(boost::asio::ip::tcp::socket socket,
                    const endpoints &endpoints,
                    Logger::Ptr logger,
                    CACHE& cache,
                    bool enable_cache = true)
            try : socket_(std::move(socket)), endpoints_(endpoints), logger(logger), cache(cache), enable_cache(enable_cache) {
#ifdef DEBUG
            logger->log(Level::Debug, "HttpSession object created");
#endif
        } catch (...) {
            logger->log(Level::Error, "Failed to create HttpSession object");
        }

        void start() override {
            do_read();
        }

        ~HttpSession() {
#ifdef DEBUG
            logger->log(Level::Debug, "HttpSession object destroyed");
#endif
        }

    private:
        void do_read() {
            auto self = shared_from_this();
            boost::asio::async_read_until(
                    socket_, request_, "\r\n\r\n",
                    [this, self](const boost::system::error_code &ec, std::size_t bytes_transferred) {
                        if (!ec) {
                            std::istream request_stream(&request_);
                            std::string request_line;
                            std::getline(request_stream, request_line);

                            std::istringstream iss(request_line);
                            std::string method, path, version;
                            iss >> method >> path >> version;

                            auto it = endpoints_.find(path);
                            if (it != endpoints_.end() && (method == "GET" ? Method::GET : Method::POST) == it->second.second) {
#ifdef DEBUG
                                logger->log(Level::Debug, "Endpoint " + path + " of type " + method + " found");
#endif
                                if (enable_cache && cache.find(method) != cache.end()) {
                                    do_write(cache[method]);
                                    logger->log(Level::Info, "Endpoint " + path + " of type " + method + " responsing...");
                                } else {
                                    std::string body = std::move(getBody(it->second.first, logger));
                                    std::string response = Templates::Responses::OK(body);
                                    do_write(response);
                                    logger->log(Level::Info, "Endpoint " + path + " of type " + method + " responsing...");
                                    if (enable_cache) {
                                        cache.insert(std::make_pair(method, response));
#ifdef DEBUG
                                        logger->log(Level::Debug, "Endpoint " + path + " of type " + method + " added to the cache");
#endif
                                    }
                                }
                            } else {
                                do_write(Templates::Responses::NOT_OK());
                                logger->log(Level::Error, "No endpoint with name " + path + " and method " + method);
                            }
                        } else {
                            logger->log(Level::Error, "Internal error in do_read() function: " + ec.message());
                        }
                    });
        }

        void do_write(const std::string &response) {
            auto self = shared_from_this();
            boost::asio::async_write(socket_, boost::asio::buffer(response),
                                     [this, self](const boost::system::error_code &ec, std::size_t length) {
                                         if (!ec) {
                                             boost::system::error_code ignored_ec;
                                             socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored_ec);
#ifdef DEBUG
                                             logger->log(Level::Debug, "do_write() ran successfully");
#endif
                                         } else {
                                             logger->log(Level::Error, "Internal boost error of code " + ec.message() + "; Stopping the server.");
                                         }
                                     });
        }

        boost::asio::ip::tcp::socket socket_;
        boost::asio::streambuf request_;
        const endpoints &endpoints_;
        const bool enable_cache;
        Logger::Ptr logger;
        CACHE& cache;
    };

    class HttpServer : Interfaces::HttpServerInterface {
    public:
        HttpServer(boost::asio::io_context &io_context,
                   Logger::Ptr logger,
                   CACHE& cache,
                   short port = 8080,
                   bool enable_cache = true)
                try : acceptor_(io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
                      socket_(io_context),
                      enable_cache(enable_cache),
                      logger(logger),
                      cache(cache)
        {
            do_accept();
#ifdef DEBUG
            logger->log(Level::Debug, "HttpServer object created");
#endif
        } catch (...) {
            logger->log(Level::Critical, "Failed to create HttpServer object");
        }

        ~HttpServer() {
#ifdef DEBUG
            logger->log(Level::Debug, "HttpServer object destroyed");
#endif
        }

        /// @param path - the endpoint path from the root page, e.g. "/"(root page), "/hello", "data"
        /// @param response - the full response page in string format (so generate the text beforehand)
        /// @param method - the method of the request; now "GET" & "POST" supported
        void addEndpoint(const std::string &path, const std::string &response, Method method) override {
            endpoints_[path] = std::make_pair(response, method);
        }

        typedef std::shared_ptr<HttpServer> Ptr;

    private:
        void do_accept() {
            acceptor_.async_accept(socket_,
                                   [this](const boost::system::error_code &ec) {
                                       if (!ec) {
                                           std::make_shared<HttpSession>(std::move(socket_), endpoints_, logger, cache, enable_cache)->start();
#ifdef DEBUG
                                           logger->log(Level::Debug, "do_accept() ran successfully");
#endif
                                       } else {
                                           logger->log(Level::Error, "Internal error in do_accept() function: " + ec.message());
                                       }
                                       do_accept();
                                   });
        }

        boost::asio::ip::tcp::acceptor acceptor_;
        boost::asio::ip::tcp::socket socket_;
        endpoints endpoints_;
        const bool enable_cache;
        Logger::Ptr logger;
        CACHE& cache;
    };

    class RESTAPIAPP : Interfaces::RESTAPIAPPInterface {
    public:
        RESTAPIAPP(uint32_t port = 8080, const std::string& logfileName="log.txt")
        try {
            logger = std::make_shared<Logger>(logfileName);
            server = std::make_shared<HttpServer>(io_context, logger, cache, port);
#ifdef DEBUG
            logger->log(Level::Debug, "RESTAPIAPP object created");
#endif
        } catch (...) {
            std::cerr << getPrefix(Level::Critical) << " Failed to build RESTAPIAPP object";
        }

        ~RESTAPIAPP() {
#ifdef DEBUG
            logger->log(Level::Debug, "RESTAPIAPP object destroyed");
#endif
        }

        void AddEndpoint(const std::string &path, const std::string &response, const std::string &method="GET") override {
#ifdef DEBUG
            logger->log(Level::Debug, "Enpoint " + path + " with method " + method + " added");
#endif
            server->addEndpoint(path, response, method == "GET" ? Method::GET : Method::POST);
        }

        void RunServer() noexcept override {
            std::string exception_message = "Failed to run the server; ";
            try {
                io_context.run();
                logger->log(Level::Info, "Server starting");
            } catch (const std::exception &e) {
                logger->log(Level::Critical, exception_message + e.what());
            } catch (const boost::exception &e) {
                logger->log(Level::Critical, exception_message + boost::diagnostic_information(e));
            } catch (...) {
                logger->log(Level::Critical, exception_message);
            }
        }

        void StopServer() noexcept override {
            std::string exception_message = "Failed to stop the server; ";
            try {
                io_context.stop();
                logger->log(Level::Info, "Server stopping");
            } catch (const std::exception &e) {
                logger->log(Level::Critical, exception_message + e.what());
            } catch (const boost::exception &e) {
                logger->log(Level::Critical, exception_message + boost::diagnostic_information(e));
            } catch (...) {
                logger->log(Level::Critical, exception_message);
            }
        }

    private:
        boost::asio::io_context io_context;
        HttpServer::Ptr server;
        Logger::Ptr logger;
        CACHE cache;  // @TODO later: wrap into a separate class & make it LRU cache, now possible memory overflow
    };
}// namespace Utils


///// Usage Example /////
/*

#include "HttpRESTAPI.hpp"
int main()
{
    using namespace Utils;
    RESTAPIAPP app(8080);
    app.AddEndpoint("/data", "Some data!", "GET");
    app.AddEndpoint("/data_from_file", "@file:/Users/egorfortov/CLionProjects/HTTP_Server_Egor_Fortov/index.html", "GET");  // better to set full path
    app.AddEndpoint("/submit", "Submitted!", "POST");
    app.RunServer();

    return 0;
}

*/