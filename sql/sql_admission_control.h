/* Copyright (c) 2016, Facebook. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

/*
 * sql_admission_control.h/cc
 *
 * This module handles multi-tenancy resource allocation on the server side.
 * Functions will call into multi-tenancy plugin interfaces (if installed) to
 * make a decision of whether or not a resource can be allocated. Connections
 * and query limits are among the resources multi-tenancy plugin allocates.
 * Note that admission control is now part of the multi-tenancy module to
 * hanlde query throttling.
 *
 * The isolation level is defined by an entity. The entity could be a database,
 * a user, or any thing that multi-tenancy plugin defines to isolate the
 * resource allocation.
 *
 * See sql_admission_control.cc for implementation.
 */

#ifndef _sql_admission_control_h
#define _sql_admission_control_h

#include "my_md5.h"
#include "my_sqlcommand.h"
#include "my_systime.h"
#include "my_thread_local.h"
#include "mysql/components/services/mysql_cond_bits.h"
#include "mysql/components/services/mysql_rwlock_bits.h"
#include "mysql/components/services/psi_cond_bits.h"
#include "mysql/components/services/psi_rwlock_bits.h"
#include "mysql/components/services/psi_stage_bits.h"
#include "mysql/psi/mysql_mutex.h"

#include <array>
#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/**
  Forward declarations.
*/
class Ac_info;

extern bool opt_admission_control_by_trx;
extern ulonglong admission_control_filter;
extern char *admission_control_weights;
extern ulonglong admission_control_wait_events;
extern ulonglong admission_control_yield_freq;
extern bool admission_control_multiquery_filter;
extern ulong admission_control_errors_size;

class AC;
class THD;
struct TABLE_LIST;
class Item;

extern AC *db_ac;

#ifdef HAVE_PSI_INTERFACE
extern PSI_stage_info stage_admission_control_enter;
extern PSI_stage_info stage_admission_control_exit;
extern PSI_stage_info stage_waiting_for_admission;
extern PSI_stage_info stage_waiting_for_readmission;
#endif

// The contents here must match entries in admission_control_filter_names array
enum enum_admission_control_filter {
  ADMISSION_CONTROL_ALTER,
  ADMISSION_CONTROL_BEGIN,
  ADMISSION_CONTROL_COMMIT,
  ADMISSION_CONTROL_CREATE,
  ADMISSION_CONTROL_DELETE,
  ADMISSION_CONTROL_DROP,
  ADMISSION_CONTROL_INSERT,
  ADMISSION_CONTROL_LOAD,
  ADMISSION_CONTROL_SELECT,
  ADMISSION_CONTROL_SET,
  ADMISSION_CONTROL_REPLACE,
  ADMISSION_CONTROL_ROLLBACK,
  ADMISSION_CONTROL_TRUNCATE,
  ADMISSION_CONTROL_UPDATE,
  ADMISSION_CONTROL_SHOW,
  ADMISSION_CONTROL_USE,
  ADMISSION_CONTROL_END = 64
};

enum enum_admission_control_wait_events {
  ADMISSION_CONTROL_THD_WAIT_SLEEP = (1U << 0),
  ADMISSION_CONTROL_THD_WAIT_ROW_LOCK = (1U << 1),
  ADMISSION_CONTROL_THD_WAIT_META_DATA_LOCK = (1U << 2),
  ADMISSION_CONTROL_THD_WAIT_INNODB_CONC = (1U << 3),
  ADMISSION_CONTROL_THD_WAIT_NET_IO = (1U << 4),
  ADMISSION_CONTROL_THD_WAIT_YIELD = (1U << 5),
  ADMISSION_CONTROL_THD_WAIT_COMMIT = (1U << 6),
};

enum enum_admission_control_request_mode {
  AC_REQUEST_NONE,
  AC_REQUEST_QUERY,
  AC_REQUEST_QUERY_READMIT_LOPRI,  // Add admission modes above readmission.
  AC_REQUEST_QUERY_READMIT_HIPRI,
};

int multi_tenancy_add_connection(THD *, const char *);
int multi_tenancy_close_connection(THD *);
int multi_tenancy_admit_query(
    THD *, enum_admission_control_request_mode mode = AC_REQUEST_QUERY);
int multi_tenancy_exit_query(THD *);
int fill_ac_queue(THD *thd, TABLE_LIST *tables, Item *cond);
int fill_ac_entities(THD *thd, TABLE_LIST *tables, Item *cond);
int fill_ac_errors(THD *thd, TABLE_LIST *tables, Item *cond);
bool filter_command(enum_sql_command sql_command);

/**
  Flight recorder base record all other records should be derived from.
*/
struct Fr_record {
  ulonglong timestamp_us{0};
};

/**
  Flight recorder implemented as a ring buffer.

  @param record Record type derived from Fr_record.
*/
template <typename Record>
class Flight_recorder {
 public:
  // Constructors.
  Flight_recorder(uint size) : ring_buffer(size) {
    assert((size & (size - 1)) == 0);
  }
  Flight_recorder(const Flight_recorder &fr)
      : ring_buffer(fr.ring_buffer), pos(fr.pos.load()) {}

  // Insert record in flight recorder.
  void insert(Record &r) {
    static_assert(std::is_base_of<Fr_record, Record>::value);

    r.timestamp_us = my_micro_time();
    uint p = pos++ % ring_buffer.size();
    ring_buffer[p] = r;
  }

  // Iterator.
  class Iter {
   public:
    // Access to current record.
    const Record &get() { return flight_rec.ring_buffer[pos]; }

    // Move to next record.
    bool move() {
      bool res = remaining != 0;
      if (res) {
        if (pos == 0) {
          pos = flight_rec.ring_buffer.size() - 1;
        } else {
          --pos;
        }
        --remaining;
      }
      return res;
    }

   private:
    // Constructors.
    Iter(Flight_recorder &fr, uint p) : flight_rec(fr), pos(p) {
      // Has ring buffer been ever wrapped around?
      auto last_index = flight_rec.ring_buffer.size() - 1;
      if (flight_rec.ring_buffer[last_index].timestamp_us) {
        remaining = flight_rec.ring_buffer.size();
      } else {
        remaining = pos;
      }
    }

    // Flight recorder.
    const Flight_recorder &flight_rec;

    // Current position.
    uint pos;

    // Number of records left to iterate.
    uint remaining;

    // Only flight recorder can create iterators.
    friend class Flight_recorder;
  };

  // Return iterator over records.
  Iter get_iter() { return Iter(*this, pos % ring_buffer.size()); }

 private:
  // Ring buffer holding records.
  std::vector<Record> ring_buffer;

  // Position of the next available record.
  std::atomic<uint> pos{0};

  // Iterator has access to the internals.
  friend class Iter;
};

/**
  Per-thread information used in admission control.
*/
struct st_ac_node {
  st_ac_node(const st_ac_node &) = delete;
  st_ac_node &operator=(const st_ac_node &) = delete;
  mysql_mutex_t lock;
  mysql_cond_t cond;
  // Note that running/queued cannot be simultaneously true.
  bool running;  // whether we need to decrement from running_queries
  bool queued;   // whether current node is queued. pos is valid iff queued.
  std::list<std::shared_ptr<st_ac_node>>::iterator pos;
  // Count of admissions for a query.
  ulong admissions;
  // The queue that this node belongs to.
  long queue;
  // The THD owning this node.
  THD *thd;
  // The ac_info this node belongs to.
  std::shared_ptr<Ac_info> ac_info;
  st_ac_node(THD *thd_arg);
  ~st_ac_node();
};

using st_ac_node_ptr = std::shared_ptr<st_ac_node>;

const ulong MAX_AC_QUEUES = 10;
/**
  Represents a queue, and its associated stats.
*/
struct Ac_queue {
  // The list representing the queue.
  std::list<st_ac_node_ptr> queue;
  inline size_t waiting_queries() const { return queue.size(); }
  // Track number of running queries.
  unsigned long running_queries = 0;
  // Track number of rejected queries.
  unsigned long aborted_queries = 0;
  // Track number of timed out queries.
  unsigned long timeout_queries = 0;
  // Track number of high priority enqueues.
  ulonglong high_pri_enqueues = 0;
  // Track number of low priority enqueues.
  ulonglong low_pri_enqueues = 0;
};

enum class Ac_result {
  AC_ADMITTED,  // Admitted
  AC_ABORTED,   // Rejected because queue size too large
  AC_TIMEOUT,   // Rejected because waiting on queue for too long
  AC_KILLED     // Killed while waiting for admission
};

/**
  Admission control entity flight record for errors.
*/
struct Ac_error_record : public Fr_record {
  struct Ac_queue_stats {
    // The number of currently waiting queries.
    ulong waiting;
    // The number of currently running queries.
    ulong running;
  };
  // THD ID of the query getting this error.
  my_thread_id thread_id;
  // Admission control error result.
  Ac_result res;
  // Admission control enqueue mode.
  enum_admission_control_request_mode mode;
  // Timestamp of last admission control exit prior to this error.
  ulonglong last_exit_timestamp_us;
  // Queue id of this query.
  ulong queue;
  // The number of high priority enqueues after this query was enqueued.
  ulong queue_boosts;
  // The number of admission control exits after this query was enqueued.
  ulong exits;
  // Total number of (re)admissions for this query.
  ulong admissions;
  // Per queue stats.
  std::array<Ac_queue_stats, MAX_AC_QUEUES> stats;
  // SQL ID of the query getting this error.
  digest_key sql_id;
};

/**
  Class used in admission control.

  Every entity (database or table or user name) will have this
  object created and stored in the global map AC::ac_map.

*/
class Ac_info {
  friend class AC;
  friend int fill_ac_queue(THD *thd, TABLE_LIST *tables, Item *cond);
  friend int fill_ac_entities(THD *thd, TABLE_LIST *tables, Item *cond);
  friend int fill_ac_errors(THD *thd, TABLE_LIST *tables, Item *cond);

  // Queues
  std::array<Ac_queue, MAX_AC_QUEUES> queues{};

  // Entity name used as key in ac_info map.
  std::string entity;

  // Count for waiting queries in queues for this Ac_info.
  unsigned long waiting_queries = 0;
  // Count for running queries in queues for this Ac_info.
  unsigned long running_queries = 0;
  // Count for rejected queries in queues for this Ac_info.
  unsigned long aborted_queries = 0;
  // Count for timed out queries in queues for this Ac_info.
  unsigned long timeout_queries = 0;

  // Count for current connections.
  unsigned long connections = 0;
  // Stats for rejected connections.
  ulonglong rejected_connections = 0;

  // Protects Ac_info.
  mysql_mutex_t lock;

  // Flight recorder for errors.
  Flight_recorder<Ac_error_record> error_recorder;

  // Track number of high priority enqueues.
  ulonglong high_pri_enqueues = 0;
  // Track number of low priority enqueues.
  ulonglong low_pri_enqueues = 0;
  // Track number of admission control exits.
  ulonglong exits = 0;
  // Timestamp of the last admission control exit.
  ulonglong last_exit_timestamp_us = 0;

 public:
  Ac_info(const std::string &);
  ~Ac_info();
  // Disable copy constructor.
  Ac_info(const Ac_info &) = delete;
  Ac_info &operator=(const Ac_info &) = delete;

  // Accessors.
  const std::string &get_entity() const;

  // Log AC error into flight recorder.
  void log_error(THD *thd, Ac_result res, ulong queue, ulonglong prev_high_pri,
                 ulonglong prev_exits,
                 enum_admission_control_request_mode mode);
};

using Ac_info_ptr = std::shared_ptr<Ac_info>;

/**
  Global class used to enforce per admission control limits.
*/
class AC {
  friend int fill_ac_queue(THD *thd, TABLE_LIST *tables, Item *cond);
  friend int fill_ac_entities(THD *thd, TABLE_LIST *tables, Item *cond);
  friend int fill_ac_errors(THD *thd, TABLE_LIST *tables, Item *cond);

  // This map is protected by the rwlock LOCK_ac.
  using Ac_info_ptr_container = std::unordered_map<std::string, Ac_info_ptr>;
  Ac_info_ptr_container ac_map;
  // Variables to track global limits
  ulong max_running_queries = 0;
  ulong max_waiting_queries = 0;
  ulong max_connections = 0;

  std::array<unsigned long, MAX_AC_QUEUES> weights{};
  /**
    Protects the above variables.

    Locking order followed is LOCK_ac, Ac_info::lock, st_ac_node::lock.
  */
  mutable mysql_rwlock_t LOCK_ac;

  std::atomic_ullong total_aborted_queries{0};
  std::atomic_ullong total_timeout_queries{0};
  std::atomic_ullong total_rejected_connections{0};

 public:
  AC();
  ~AC();

  // Disable copy constructor.
  AC(const AC &) = delete;
  AC &operator=(const AC &) = delete;

  /*
   * Removes a dropped entity info from the global map.
   */
  void remove(const char *entity);

  void insert(const std::string &entity);

  void update_max_running_queries(ulong val);

  void update_max_waiting_queries(ulong val);

  void update_max_connections(ulong val);

  ulong get_max_running_queries() const;

  ulong get_max_waiting_queries() const;

  Ac_result admission_control_enter(THD *, enum_admission_control_request_mode);
  void admission_control_exit(THD *);
  bool wait_for_signal(THD *, st_ac_node_ptr &, const Ac_info_ptr &ac_info,
                       enum_admission_control_request_mode);
  static void enqueue(THD *thd, Ac_info_ptr ac_info,
                      enum_admission_control_request_mode);
  static void dequeue(THD *thd, Ac_info_ptr ac_info);
  static void dequeue_and_run(THD *thd, Ac_info_ptr ac_info);

  Ac_result add_connection(THD *, const char *);
  void close_connection(THD *);

  int update_queue_weights(char *str);

  ulonglong get_total_aborted_queries() const { return total_aborted_queries; }
  ulonglong get_total_timeout_queries() const { return total_timeout_queries; }
  ulonglong get_total_rejected_connections() const {
    return total_rejected_connections;
  }

  ulong get_total_running_queries() const;

  ulong get_total_waiting_queries() const;
};

/**
  @brief Class temporarily holding existing resources during ac_info switch.
*/
class Ac_switch_guard {
  THD *thd;
  std::shared_ptr<Ac_info> ac_info;
  // Whether switch succeeded or not.
  bool committed = false;
  // Is switch actually needed?
  bool do_switch = true;

 public:
  Ac_switch_guard(THD *);
  ~Ac_switch_guard();
  void commit() {
    // If switch is not needed then commit is ignored, and ac_info will be
    // restored.
    if (do_switch) committed = true;
  }
  int add_connection(const char *new_db);
};

#endif /* _sql_admission_control_h */
