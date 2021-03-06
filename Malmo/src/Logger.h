// --------------------------------------------------------------------------------------------------
//  Copyright (c) 2016 Microsoft Corporation
//  
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//  associated documentation files (the "Software"), to deal in the Software without restriction,
//  including without limitation the rights to use, copy, modify, merge, publish, distribute,
//  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//  
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//  
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
//  NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//  DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// --------------------------------------------------------------------------------------------------

#ifndef _MALMO_LOGGER_H_
#define _MALMO_LOGGER_H_

// Ideally we'd use boost::log rather than reinventing the wheel here, but due to our requirements to be able
// to use boost statically, boost::log is not an option:
// "If your application consists of more than one module (e.g. an exe and one or several dll's) that use Boost.Log,
// the library must be built as a shared object. If you have a single executable or a single module that works
// with Boost.Log, you may build the library as a static library."
// (from http://www.boost.org/doc/libs/1_64_0/libs/log/doc/html/log/installation/config.html )

// The logger can act straight away (when Logger::print is called), but for performance reasons it is better to
// buffer the log messages and dump them en masse periodically, preferably in a background thread.
// Unfortuantely there are some hairy object lifetime difficulties involved in keeping our *own* thread, since the
// logger is a static singleton, which will be destroyed after main() has exited, which, with VS2013 at least,
// will cause a deadlock our thread hasn't already joined() - 
// see https://connect.microsoft.com/VisualStudio/feedback/details/747145 for example.
// See comments below in log_spooler() for details.


// This logger partly owes its genesis to this Dr Dobbs article:
// http://www.drdobbs.com/parallel/more-memory-for-dos-exec/parallel/a-lightweight-logger-for-c/240147505
// and the associated github project by Filip Janiszewski - https://github.com/fjanisze/logger
// Note however that the code in github will not work on Windows (under VS2013 at least) for several reasons -
// firstly for the thread lifetime issue above, secondly because of the way VS treats std::move of string literals -
// see http://stackoverflow.com/questions/34160614/stdmove-of-string-literal-which-compiler-is-correct for an example.
// Thirdly VS2013 doesn't support the use of the ATOMIC_FLAG_INIT initialisation macro (eg see
// https://connect.microsoft.com/VisualStudio/feedback/details/800243/visual-studio-2013-rc-std-atomic-flag-regression etc.)

// Boost:
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/date_time/posix_time/posix_time_io.hpp>

// STL:
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <sstream>
#include <iostream>
#include <fstream>
#include <algorithm>

namespace malmo
{
    #define LOGERROR(...) Logger::getLogger().print<Logger::LOG_ERRORS>(__VA_ARGS__)
    #define LOGINFO(...) Logger::getLogger().print<Logger::LOG_INFO>(__VA_ARGS__)
    #define LOGFINE(...) Logger::getLogger().print<Logger::LOG_FINE>(__VA_ARGS__)
    #define LOGTRACE(...) Logger::getLogger().print<Logger::LOG_TRACE>(__VA_ARGS__)
    #define LOGSIMPLE(level, message) Logger::getLogger().print<Logger:: level >(std::string(message))
    #define LOGSECTION(level, message) LogSection<Logger:: level > log_section(message);
    #define LT(x) std::string(x)
    #define MALMO_LOGGABLE_OBJECT(name) LoggerLifetimeTracker log_tracker{ #name };

    class Logger
    {
    public:
        //! Specifies the detail that will be logged, if logging is enabled.
        enum LoggingSeverityLevel {
            LOG_OFF
            , LOG_ERRORS
            , LOG_WARNINGS
            , LOG_INFO
            , LOG_FINE
            , LOG_TRACE
            , LOG_ALL
        };

        Logger() : line_number(0), indentation(0)
        {
            this->has_backend = false;
            this->is_spooling.clear();
            this->logger_backend = new std::thread{ Logger::log_spooler, this };
        }
        ~Logger()
        {
            // Switch off logging now, to avoid complications:
            this->severity_level = LOG_OFF;
            // Let our spooling thread know that we want it to stop:
            this->is_spooling.clear();
            // Wait for it to finish (we can't use join() due to
            // the exit lock issue.)
            // In some scenarios, ExitProcess has already been called by this point,
            // and our thread will have been terminated - so we don't want to wait
            // indefinitely.
            // It would be nicer to use std::this_thread::sleep_for(...), but it's not safe
            // to call from within dllmain.
            auto start = std::chrono::system_clock::now();
            while (this->has_backend && (std::chrono::system_clock::now() - start).count() < 2.0);
            // Clear whatever is left in our buffer.
            // DON'T acquire the write_guard lock at this point - by this point
            // there should be no danger of anyone else accessing our buffer,
            // and if we got here because the process is exiting, it might not be
            // safe to use the mutex.
            clear_backlog();
            // Detach the thread:
            this->logger_backend->detach();
            // And close our file, if we have one:
            if (this->writer.is_open())
                this->writer.close();
        }
        Logger(const malmo::Logger &) = delete;

        static Logger& getLogger()
        {
            static Logger the_logger;
            return the_logger;
        }

        template<LoggingSeverityLevel level, typename...Args>
        void print(Args&&...args);
        void setSeverityLevel(LoggingSeverityLevel level) { severity_level = level; }
        void setFilename(const std::string& file)
        {
            if (this->writer.is_open())
                this->writer.close();
            this->writer.open(file, std::ofstream::out | std::ofstream::app);
        }

        //! Sets logging options for debugging.
        //! \param filename A filename to output log messages to. Will use the console if this is empty / can't be written to.
        //! \param severity_level Determine how verbose the log will be.
        static void setLogging(const std::string& filename, LoggingSeverityLevel severity_level)
        {
            Logger::getLogger().setFilename(filename);
            Logger::getLogger().setSeverityLevel(severity_level);
        }
        //! Add a single line to the log.
        //! Provided for external use - swigged/bound to allow user code
        //! to add to the log, to assist in debugging.
        //! Internal (Malmo) code should use the macros instead.
        //! \param severity_level The level for this log line.
        //! \message The message to add to the log file.
        static void appendToLog(LoggingSeverityLevel severity_level, const std::string& message)
        {
            switch (severity_level)
            {
            case LOG_ERRORS:
                Logger::getLogger().print<LOG_ERRORS>(message); break;
            case LOG_WARNINGS:
                Logger::getLogger().print<LOG_WARNINGS>(message); break;
            case LOG_INFO:
                Logger::getLogger().print<LOG_INFO>(message); break;
            case LOG_FINE:
                Logger::getLogger().print<LOG_FINE>(message); break;
            case LOG_TRACE:
                Logger::getLogger().print<LOG_TRACE>(message); break;
            case LOG_ALL:
                Logger::getLogger().print<LOG_ALL>(message); break;
            }
        }

    protected:
        template<LoggingSeverityLevel level> friend class LogSection;
        friend class LoggerLifetimeTracker;

        void indent()
        {
            std::lock_guard< std::timed_mutex > lock(write_guard);
            indentation++;
        }
        void unindent()
        {
            std::lock_guard< std::timed_mutex > lock(write_guard);
            indentation--;
        }

        static void log_spooler(Logger* logger)
        {
            logger->has_backend = true;
            logger->is_spooling.test_and_set();
            std::unique_lock<std::timed_mutex> writing_lock{ logger->write_guard, std::defer_lock };
            // Loop until the logger clears the is_spooling flag - this happens when the logger is destructed.
            do
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{ 100 });
                if (logger->log_buffer.size())
                {
                    writing_lock.lock();
                    logger->clear_backlog();
                    writing_lock.unlock();
                }
            } while (logger->is_spooling.test_and_set());
            // The logger will be waiting for us to finish spooling - let it know it can continue:
            logger->has_backend = false;
            // ON WINDOWS, VS2013:
            // If we let this function end now, we will get a nasty race condition with the CRT.
            // When the thread finishes running its main method (this function), it will enter 
            // _Cnd_do_broadcast_at_thread_exit(), and attempt to signal the news of the thread's demise
            // to anyone waiting for it. Doing this involves creating an "at_thread_exit_mutex", and adding
            // a destruction method for it ("destroy_at_thread_exit_mutex") to the DLL's atexit/_onexit list.
            // (This is a list of functions which will get called when the DLL is signalled to exit by ExitProcess.)

            // BUT:

            // Because logger is a static singleton, its destruction takes place AFTER main() exits, at
            // which point the CRT has imposed an EXIT LOCK. All exit code needs to own the exit lock before it can
            // execute. At the point when ~Logger() is called, the main thread owns this exit lock, and it won't
            // relinquish it until right before it calls ExitProcess, after going through its own atexit/_onexit list.
            // The worker thread needs to aquire this lock before it can create the at_thread_exit_mutex, so it
            // is forced to wait while everything else is destructed and cleaned before it can carry on.

            // THe end result is that the worker thread has just enough time to create and lock the mutex before
            // the main thread calls ExitProcess, which calls the DLL cleanup code, which deletes the mutex,
            // resulting in an abort() with a "mutex destroyed while busy" error.

            // To get around this, we simply go to sleep for a bit, to give the main thread a chance to exit
            // before we even have a chance to leave this function.
            std::this_thread::sleep_for(std::chrono::milliseconds(100000));
            // It'll all be over before we even wake up.
        }

        void clear_backlog()
        {
            for (auto& item : this->log_buffer)
            {
                performWrite(item);
            }
            this->log_buffer.clear();
        }

        void performWrite(const std::string& logline)
        {
            std::string str(logline);
            str.erase(std::remove(str.begin(), str.end(), '\n'), str.end());
            if (this->writer.is_open())
                this->writer << str << std::endl;
            else
                std::cout << str << std::endl;
        }

    private:
        friend void log_spooler(Logger* logger);

        template<typename First, typename... Others> void print_impl(std::stringstream&& message_stream, First&& param1, Others&&... params)
        {
            message_stream << param1;
            print_impl(std::forward<std::stringstream>(message_stream), std::move(params)...);
        }

        void print_impl(std::stringstream&& message_stream)
        {
            std::lock_guard< std::timed_mutex > lock(this->write_guard);
            this->log_buffer.push_back(message_stream.str());
        }

        LoggingSeverityLevel severity_level{ LOG_OFF };
        int line_number;
        int indentation;
        std::timed_mutex write_guard;
        std::vector<std::string> log_buffer;
        std::thread spooler;
        std::atomic_bool has_backend;
        std::atomic_flag is_spooling;
        bool terminated;
        std::ofstream writer;
        std::thread *logger_backend;
    };

    template<Logger::LoggingSeverityLevel level, typename...Args>void Logger::print(Args&&...args)
    {
        if (level > severity_level)
            return;
        std::stringstream message_stream;
        auto now = boost::posix_time::microsec_clock::universal_time();
        message_stream << now << " P "; // 'P' for 'Platform' - useful if combining logs with Mod-side.
        switch (level)
        {
        case LoggingSeverityLevel::LOG_ALL:
        case LoggingSeverityLevel::LOG_ERRORS:
            message_stream << "ERROR   ";
            break;
        case LoggingSeverityLevel::LOG_WARNINGS:
            message_stream << "WARNING ";
            break;
        case LoggingSeverityLevel::LOG_INFO:
            message_stream << "INFO    ";
            break;
        case LoggingSeverityLevel::LOG_FINE:
            message_stream << "FINE    ";
            break;
        case LoggingSeverityLevel::LOG_TRACE:
        case LoggingSeverityLevel::LOG_OFF:
            message_stream << "TRACE   ";
            break;
        }
        for (int i = 0; i < this->indentation; i++)
            message_stream << "    ";

        print_impl(std::forward<std::stringstream>(message_stream), std::move(args)...);
        this->line_number++;
    }

    template<Logger::LoggingSeverityLevel level>
    class LogSection
    {
    public:
        LogSection(const std::string& title)
        {
            Logger::getLogger().print<level>(title);
            Logger::getLogger().print<level>(std::string("{"));
            Logger::getLogger().indent();
        }
        ~LogSection()
        {
            Logger::getLogger().unindent();
            Logger::getLogger().print<level>(std::string("}"));
        }
    };

    class LoggerLifetimeTracker
    {
    public:
        LoggerLifetimeTracker(const std::string& _name) : name(_name)
        {
            addref();
        }
        LoggerLifetimeTracker(const LoggerLifetimeTracker& rhs) : name(rhs.name)
        {
            addref();
        }
        ~LoggerLifetimeTracker()
        {
            int prev_val = object_count.fetch_add(-1);
            LOGFINE(LT("Destructing "), this->name, LT(" (object count now "), prev_val - 1, LT(")"));
        }

    private:
        void addref()
        {
            int prev_val = object_count.fetch_add(1);
            LOGFINE(LT("Constructing "), this->name, LT(" (object count now "), prev_val + 1, LT(")"));
        }
        static std::atomic<int> object_count;
        std::string name;
    };
}

#endif //_MALMO_LOGGER_H_
