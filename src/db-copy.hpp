#ifndef OSM2PGSQL_DB_COPY_HPP
#define OSM2PGSQL_DB_COPY_HPP

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "osmtypes.hpp"

class pg_conn_t;

/**
 * Table information necessary for building SQL queries.
 */
struct db_target_descr_t
{
    /// Name of the target table for the copy operation.
    std::string name;
    /// Comma-separated list of rows for copy operation (when empty: all rows)
    std::string rows;
    /// Name of id column used when deleting objects.
    std::string id;

    /**
     * Check if the buffer would use exactly the same copy operation.
     */
    bool same_copy_target(db_target_descr_t const &other) const noexcept
    {
        return (this == &other) || (name == other.name && rows == other.rows);
    }

    db_target_descr_t() = default;
    db_target_descr_t(char const *n, char const *i, char const *r = "")
    : name(n), rows(r), id(i)
    {}
};

/**
 * Deleter which removes objects by id from the database.
 */
class db_deleter_by_id_t
{
    enum
    {
        // There is a trade-off here between sending as few DELETE SQL as
        // possible and keeping the size of the deletable vector managable.
        Max_entries = 1000000
    };

public:
    bool has_data() const noexcept { return !m_deletables.empty(); }

    void add(osmid_t osm_id) { m_deletables.push_back(osm_id); }

    void delete_rows(std::string const &table, std::string const &column,
                     pg_conn_t *conn);

    bool is_full() const noexcept { return m_deletables.size() > Max_entries; }

private:
    /// Vector with object to delete before copying
    std::vector<osmid_t> m_deletables;
};

/**
 * A command for the copy thread to execute.
 */
class db_cmd_t
{
public:
    enum cmd_t
    {
        Cmd_copy, ///< Copy buffer content into given target.
        Cmd_sync, ///< Synchronize with parent.
        Cmd_finish
    };

    virtual ~db_cmd_t() = default;

    cmd_t type;

protected:
    explicit db_cmd_t(cmd_t t) : type(t) {}
};

struct db_cmd_copy_t : public db_cmd_t
{
    enum
    {
        /** Size of a single buffer with COPY data for Postgresql.
         *  This is a trade-off between memory usage and sending large chunks
         *  to speed up processing. Currently a one-size fits all value.
         *  Needs more testing and individual values per queue.
         */
        Max_buf_size = 10 * 1024 * 1024,
        /** Maximum length of the queue with COPY data.
         *  In the usual case, PostgreSQL should be faster processing the
         *  data than it can be produced and there should only be one element
         *  in the queue. If PostgreSQL is slower, then the queue will always
         *  be full and it is better to keep the queue smaller to reduce memory
         *  usage. Current value is just assumed to be a reasonable trade off.
         */
        Max_buffers = 10
    };

    /// Name of the target table for the copy operation
    std::shared_ptr<db_target_descr_t> target;
    /// actual copy buffer
    std::string buffer;

    virtual bool has_deletables() const noexcept = 0;
    virtual void delete_data(pg_conn_t *conn) = 0;

    explicit db_cmd_copy_t(std::shared_ptr<db_target_descr_t> const &t)
    : db_cmd_t(db_cmd_t::Cmd_copy), target(t)
    {
        buffer.reserve(Max_buf_size);
    }
};

template <typename DELETER>
class db_cmd_copy_delete_t : public db_cmd_copy_t
{
public:
    using db_cmd_copy_t::db_cmd_copy_t;

    /// Return true if the buffer is filled up.
    bool is_full() const noexcept
    {
        return (buffer.size() > Max_buf_size - 100) || m_deleter.is_full();
    }

    bool has_deletables() const noexcept override
    {
        return m_deleter.has_data();
    }

    void delete_data(pg_conn_t *conn) override
    {
        if (m_deleter.has_data()) {
            m_deleter.delete_rows(target->name, target->id, conn);
        }
    }

    template <typename... ARGS>
    void add_deletable(ARGS &&... args)
    {
        m_deleter.add(std::forward<ARGS>(args)...);
    }

private:
    /// Deleter class for old items
    DELETER m_deleter;
};

struct db_cmd_sync_t : public db_cmd_t
{
    std::promise<void> barrier;

    explicit db_cmd_sync_t(std::promise<void> &&b)
    : db_cmd_t(db_cmd_t::Cmd_sync), barrier(std::move(b))
    {}
};

struct db_cmd_finish_t : public db_cmd_t
{
    db_cmd_finish_t() : db_cmd_t(db_cmd_t::Cmd_finish) {}
};

/**
 * The worker thread that streams copy data into the database.
 */
class db_copy_thread_t
{
public:
    db_copy_thread_t(std::string const &conninfo);
    ~db_copy_thread_t();

    /**
     * Add another command for the worker.
     */
    void add_buffer(std::unique_ptr<db_cmd_t> &&buffer);

    /**
     * Send sync command and wait for the notification.
     */
    void sync_and_wait();

    /**
     * Finish the copy process.
     *
     * Only returns when all remaining data has been committed to the
     * database.
     */
    void finish();

private:
    void worker_thread();

    void connect();
    void disconnect();

    void write_to_db(db_cmd_copy_t *buffer);
    void start_copy(std::shared_ptr<db_target_descr_t> const &target);
    void finish_copy();
    void delete_rows(db_cmd_copy_t *buffer);

    std::string m_conninfo;
    std::unique_ptr<pg_conn_t> m_conn;

    std::thread m_worker;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cond;
    std::condition_variable m_queue_full_cond;
    std::deque<std::unique_ptr<db_cmd_t>> m_worker_queue;

    // Target for copy operation currently ongoing.
    std::shared_ptr<db_target_descr_t> m_inflight;
};

#endif // OSM2PGSQL_DB_COPY_HPP
