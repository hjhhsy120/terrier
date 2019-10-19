// Whether pin to core, only for GC now. TODO : discuss it later
#define MY_PIN_TO_CORE

#include <atomic>
#include <memory>
#include <numeric>
#include <sched.h>
#include <vector>

#include "benchmark/benchmark.h"
#include "common/scoped_timer.h"
#include "util/multithread_test_util.h"

#include <util/catalog_test_util.h>
#include "parser/expression/column_value_expression.h"
#include "portable_endian/portable_endian.h"
#include "storage/garbage_collector_thread.h"
#include "storage/index/compact_ints_key.h"
#include "storage/index/index_builder.h"
#include "storage/projected_row.h"
#include "storage/sql_table.h"
#include "transaction/transaction_context.h"
#include "transaction/transaction_manager.h"
#include "type/type_id.h"
#include "type/type_util.h"
#include "util/random_test_util.h"
#include "util/storage_test_util.h"
#include "util/test_harness.h"
//#include "util/transaction_test_util.h"

#include <iostream>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

//#include "execution/util/common.h"
#include "execution/tplclass.h"
#include "catalog/catalog.h"
#include "execution/table_generator/sample_output.h"
#include "execution/util/timer.h"
#include "execution/vm/module.h"

#include "common/thread_cpu_timer.h"

namespace terrier {

    /*
     * Benchmark for index creation time with/without other workloads
     * Now also support TPCH benchmark
     */
    class IndexBenchmark : public benchmark::Fixture {

    public:
        // Switches
        bool local_test_; // Whether it is a local test with small numbers
        bool scan_all_; // Whether to scan whole table; otherwise scan 1M at a time
        bool use_perf_; // Whether to use perf which needs getchar before and after main body
        bool pin_to_core_; // Whether to pin to core
        bool one_always_; // Whether always run a extra task on core 20, so always use hyper-threading
        bool single_test_; // Whether run a small test, work only if local_test_ is false
        bool need_index_; // Whether to use index
        bool need_tpch_; // Whether to use TPCH as other workload

        enum other_types {EMPTY, LOOP, ARRAY, ARRAY10M, INDEX, TPCH, SCAN} other_type_;
        std::string type_names_[7] = {"EMPTY", "LOOP", "ARRAY", "ARRAY10M", "INDEX", "TPCH", "SCAN"};
        enum workload_types {UINDEX, UTPCH, ULOOP, USCAN, UTABLE} workload_type_;
        std::vector <int> tpch_list_;
        int tpch_number_;
        int tpch_repeated_times_;

        // Upper bounds
        int max_times_; // run how many experiments and record average time
        int max_num_columns_;
        uint32_t max_num_inserts_;
        uint32_t max_num_threads_;
        int big_number_for_array_test_;
        uint32_t num_inserts_per_table_;
        int scan_size_kb_; // Size for each scan when scan_all_ is false, currently 1M
        uint32_t tpch_filenum_;
        uint32_t num_scan_once_; // for UTABLE to scan sql_tables_

        uint32_t fixed_core_id_;
        std::string scan_filename_;
        std::vector <bool> tpch_mode_;

        // List of values
        std::vector <std::string> tpch_filename_;
        std::vector <uint32_t> core_ids_;

        std::vector <uint32_t> num_inserts_list_;
        std::vector <uint32_t> num_threads_list_;
        std::vector <int> num_columns_list_;

        // Data and objects for index creation and TPCH
        std::vector<uint32_t> key_permutation_;
        std::default_random_engine generator_;

        std::chrono::milliseconds gc_period_;
        storage::GarbageCollector *gc_;
        storage::GarbageCollectorThread *gc_thread_;

        storage::BlockStore block_store_{100000, 100000};
        storage::RecordBufferSegmentPool buffer_pool_{1000000, 1000000};
        std::vector <storage::SqlTable *> sql_tables_;
        transaction::TimestampManager tm_manager_{};
        transaction::DeferredActionManager da_manager_{&tm_manager_};
        transaction::TransactionManager txn_manager_{&tm_manager_, &da_manager_, &buffer_pool_, true, nullptr};
        std::vector<catalog::col_oid_t> col_oids_;
        execution::exec::SampleOutput sample_output_;
        catalog::db_oid_t db_oid_;

        // For TPCH benchmark, use another table
        execution::exec::SampleOutput sample_output_benchmark_;
        catalog::db_oid_t db_oid_benchmark_;

        std::unique_ptr<catalog::Catalog> catalog_pointer_;

        /*
         * Generate the tables for index creation
         * Used in Setup function
         */
        void GenerateTablesForIndex() {
            // Prepare data for tables for indexing
            key_permutation_.clear();
            uint32_t total_num_inserts = max_num_inserts_ * 2;
            key_permutation_.resize(total_num_inserts);
            for (uint32_t i = 0; i < total_num_inserts; i++) {
                key_permutation_[i] = i;
            }
            // for random data
            std::shuffle(key_permutation_.begin(), key_permutation_.end(), generator_);

            // Initialization of tables for indexing
            char column_name[] = "A_attribute";
            std::vector<catalog::Schema::Column> columns;
            col_oids_.clear();

            for (int i = 0; i < max_num_columns_; i++) {
                auto col = catalog::Schema::Column(
                        "attribute", type::TypeId::BIGINT, false,
                        parser::ConstantValueExpression(type::TransientValueFactory::GetNull(type::TypeId::BIGINT)));
                StorageTestUtil::ForceOid(&(col), catalog::col_oid_t(i));
                columns.push_back(col);
                col_oids_.push_back(catalog::col_oid_t(i));
                column_name[0]++;
            }

            const catalog::Schema table_schema{catalog::Schema(columns)};

            // Insert data into SqlTable for indexing
            // Currently we do not have parallel scan on one table, so we have several tables for the threads to scan concurrently
            for (int table_index = 0; table_index < (int)max_num_threads_ * 2 - 2; table_index++) {
                storage::SqlTable *sql_table = new storage::SqlTable(&block_store_, table_schema);
                //, catalog::table_oid_t(1));
                storage::ProjectedRowInitializer tuple_initializer{
                        sql_table->InitializerForProjectedRow(col_oids_)};
                //storage::ProjectedRowInitializer tuple_initializer =
                //        storage::ProjectedRowInitializer::Create(std::vector<uint8_t>{1}, std::vector<uint16_t>{1});

                auto *const insert_txn = txn_manager_.BeginTransaction();

                for (uint32_t k = 0; k < num_inserts_per_table_; k++) {
                    uint32_t i = key_permutation_[table_index * num_inserts_per_table_ + k];
                    auto *const insert_redo =
                            insert_txn->StageWrite(CatalogTestUtil::TEST_DB_OID, CatalogTestUtil::TEST_TABLE_OID,
                                                   tuple_initializer);
                    auto *const insert_tuple = insert_redo->Delta();
                    for (uint16_t j = 0; j < (uint16_t)max_num_columns_; j++)
                        *reinterpret_cast<int64_t *>(insert_tuple->AccessForceNotNull(j)) = (int64_t)i;
                    sql_table->Insert(insert_txn, insert_redo);
                }
                txn_manager_.Commit(insert_txn, transaction::TransactionUtil::EmptyCallback, nullptr);
                sql_tables_[table_index] = sql_table;
            }
            std::cout << "Finished building tables for index" << std::endl;
        }

        /*
         * Set upper bounds and lists for index benchmark
         */
        void SetIndex() {
            max_num_columns_ = 6;
            num_inserts_list_.clear();
            num_threads_list_.clear();
            num_columns_list_.clear();

            if (local_test_) { // local test for PC: use very small #threads, #insertions...
                max_num_inserts_ = 10000;
                num_inserts_list_.push_back(max_num_inserts_);
                num_threads_list_.push_back(2);
                num_columns_list_.push_back(max_num_columns_);
            } else { // for the server
                max_num_inserts_ = 67108864;
                if (single_test_) { // Small test for correctness of code
                    num_inserts_list_.push_back(50000000);
                    //for (int i = 17; i >= 4; i -= 3)
                    num_threads_list_.push_back(1);
                    num_columns_list_.push_back(3);
                } else { // full experiment data collection
                    const uint32_t num_inserts_list[20] = {1, 16, 256, 1024, 2048, 4096, 8192, 16384,
                                                           32768, 65536, 131072, 262144, 524288,
                                                           1048576, 2097152, 4194304, 8388608,
                                                           16777216, 33554432, 67108864};
                    for (int i = 0; i < 20; i++)
                        num_inserts_list_.push_back(num_inserts_list[i]);
                    const uint32_t num_threads_list[8] = {18, 16, 12, 10, 9, 6, 4, 1};
                    if (one_always_) { // for hyper-threading performance with small #threads
                        for (int i = 4; i < 8; i++)
                            num_threads_list_.push_back(num_threads_list[i]);
                    } else { // not use hyper-threading when #threads is small than 18
                        for (int i = 0; i < 8; i++)
                            num_threads_list_.push_back(num_threads_list[i]);
                    }
                    const int num_columns_list[4] = {1, 2, 4, 6};
                    for (int i = 0; i < 4; i++)
                        num_columns_list_.push_back(num_columns_list[i]);
                }
            }
            core_ids_.clear();
            for (int i = 0; i <= 8; i++)
                core_ids_.push_back(i);
            if (!one_always_)
                core_ids_.push_back(20);
            for (int i = 21; i <= 28; i++)
                core_ids_.push_back(i);
        }

        /*
         * Set upper bounds and lists for other benchmark
         */
        void SetOtherBenchmark() {
            max_num_columns_ = 6;
            num_inserts_list_.clear();
            num_columns_list_.clear();
            if (local_test_) { // local test for PC: use very small #threads, #insertions...
                max_num_inserts_ = 10000;
            } else { // for the server
                max_num_inserts_ = 1000000;
            }
            num_inserts_list_.push_back(max_num_inserts_);
            num_threads_list_.push_back(1);
            num_columns_list_.push_back(max_num_columns_);
            core_ids_.clear();
            if (local_test_) {
                core_ids_.push_back(0);
                core_ids_.push_back(1);
                core_ids_.push_back(3);
                core_ids_.push_back(2);
            } else {
                for (int i = 0; i <= 8; i++)
                    core_ids_.push_back(i);
                for (int i = 21; i <= 28; i++)
                    core_ids_.push_back(i);
                core_ids_.push_back(20);
            }
        }

        /*
         * Setup function for google benchmark
         */
        void SetUp(const benchmark::State &state) final {

            // Switches
            local_test_ = false;
            scan_all_ = false;
            use_perf_ = false;
            pin_to_core_ = true;
            one_always_ = false;
            single_test_ = true;
            need_index_ = true;
            need_tpch_ = true;

            other_type_ = LOOP;
            workload_type_ = UINDEX;

            // Initialization of upper bounds and lists
            max_times_ = 3;
            if (local_test_) {
                max_num_threads_ = 4;
                fixed_core_id_ = 2;
                big_number_for_array_test_ = 1 << 25;
            } else {
                max_num_threads_ = 18;
                fixed_core_id_ = 20;
                big_number_for_array_test_ = 1 << 28;
            }
            if (other_type_ == ARRAY10M)
                big_number_for_array_test_ = 10000000;
            if (workload_type_ == UINDEX)
                SetIndex();
            else
                SetOtherBenchmark();

            num_inserts_per_table_ = max_num_inserts_ / max_num_threads_ + 1;
            scan_size_kb_ = 1000; // useless if scan_all_ is true
            num_scan_once_ = 2048;

            // set up TPL file names
            const std::string filenames[4] = {"../../tpl_tables/sample_tpl/tpch_q1.tpl",
                                              "../../tpl_tables/sample_tpl/tpch_q4.tpl",
                                              "../../tpl_tables/sample_tpl/tpch_q5.tpl",
                                              "../../tpl_tables/sample_tpl/tpch_q6.tpl"};
            tpch_filename_.clear();
            tpch_filenum_ = 4;
            for (int i = 0; i < 4; i++)
                tpch_filename_.push_back(filenames[i]);
            scan_filename_ = "../sample_tpl/scanall.tpl";
            for (int i = 0; i < 4; i++)
                tpch_list_.push_back(i);
            tpch_mode_.resize(3);
            tpch_mode_[0] = true; // interpret
            tpch_mode_[1] = false; // adaptive
            tpch_mode_[2] = false; // jit
            tpch_repeated_times_ = 1;

            // set up sequence of cores and GC
            std::chrono::milliseconds gc_period{10};
            gc_period_ = gc_period;
            gc_ = new storage::GarbageCollector(&tm_manager_, &da_manager_, &txn_manager_, DISABLED);
            gc_thread_ = new storage::GarbageCollectorThread(gc_, gc_period_);
            sql_tables_.resize(max_num_threads_ * 2 - 2);

            // If using INDEX workload, generate the tables
            if (need_index_)
                GenerateTablesForIndex();
            // If using TPCH or SCAN workload, initialize the table for tpl queries
            if (need_tpch_ || workload_type_ == UTPCH || workload_type_ == USCAN) {
                catalog_pointer_ = std::make_unique<catalog::Catalog>(&txn_manager_, &block_store_);
                const char *cmd0 = "tpl";
                // currently cmd1 is not necessary
                // const char * cmd1 = "-output-name=tpch_q1";
                const char *cmd2 = "-sql";
                const char *cmd3 = "../../tpl_tables/sample_tpl/tpch_q1.tpl";
                const char *cmd_for_tpch[3] = {cmd0, cmd2, cmd3};

                execution::TplClass::InitTplClass(3, (char **) cmd_for_tpch);
            }
            if (workload_type_ == UTPCH || workload_type_ == USCAN) {
                execution::TplClass::BuildDb(txn_manager_, block_store_, sample_output_benchmark_, db_oid_benchmark_,
                                             *catalog_pointer_, "benchmark_db", "../../tpl_tables/tables/");
            }
            if (need_tpch_) {
                execution::TplClass::BuildDb(txn_manager_, block_store_, sample_output_, db_oid_,
                                             *catalog_pointer_, "other_db", "../../tpl_tables/tables/");
            }
            tpch_number_ = 0;

            if ((workload_type_ == UTPCH || workload_type_ == ULOOP || workload_type_ == USCAN || workload_type_ == UTABLE) && single_test_ && !local_test_) { // Small test for correctness of code
                other_type_ = EMPTY;
                one_always_ = false;
                max_num_threads_ = 18;
                tpch_number_ = 0;
                tpch_repeated_times_ = 1;
            }
        }

        /*
         * Teardown function for google benchmark
         */
        void TearDown(const benchmark::State &state) final {
            for (int table_index = 0; table_index < (int)max_num_threads_ * 2 - 2; table_index++) {
                delete sql_tables_[table_index];
            }
            if (need_tpch_) {
                catalog_pointer_->TearDown();
                execution::TplClass::ShutdownTplClass();
            }
            delete gc_thread_;
            delete gc_;
        }

        /*
         * pin the thread to the core
         * core_id: logical core id
         */
        inline void MyPinToCore(uint32_t core_id) {
            cpu_set_t cpu_set;
            CPU_ZERO(&cpu_set);
            CPU_SET(core_id, &cpu_set);
            int ret = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set), &cpu_set);
            if(ret != 0) {
                fprintf(stderr, "CPU setting failed...\n");
                exit(1);
            }
        }

        /*
         * function with a loop of * and +
         */
        void LoopFunction(std::atomic <bool> *unfinished) {
            volatile int x = 1 ;
            do {
                for (int i = 0; i < (1 << 30); i++)
                    x = x * 3 + 7;
            } while(*unfinished);
            volatile int y UNUSED_ATTRIBUTE = x;
        }

        /*
         * function with array enumeration
         */
        void ArrayFunction(std::atomic <bool> *unfinished, std::vector <int> & my_array, int array_length) {
            while(*unfinished) {
                for (int i = 0; i < array_length; i++)
                    my_array[i] = my_array[i] * 3 + 7;
            }
        }

        /*
         * Initialize an index object
         */
        storage::index::Index * IndexInit(int num_columns) {
            // Initialize the index
            catalog::IndexSchema default_schema;
            std::vector<catalog::IndexSchema::Column> keycols;

            for (int i = 0; i < num_columns; i++) {
                keycols.emplace_back(
                        "", type::TypeId::BIGINT, false,
                        parser::ColumnValueExpression(catalog::db_oid_t(0), catalog::table_oid_t(0),
                                                      catalog::col_oid_t(i)));
                StorageTestUtil::ForceOid(&(keycols[i]), catalog::indexkeycol_oid_t(i));
            }
            default_schema = catalog::IndexSchema(keycols, false, false, false, true);

            // BwTreeIndex
            return (storage::index::IndexBuilder()
            .SetConstraintType(storage::index::ConstraintType::DEFAULT)
                    .SetKeySchema(default_schema)
                    .SetOid(catalog::index_oid_t(2)))
                    .Build();
        }

        /*
         * function of index insertion for each thread
         */
        void IndexInsertion(uint32_t worker_id, storage::index::Index * default_index,
                            uint32_t num_inserts, int num_columns, uint32_t num_threads, double *insert_time_ms) {
            double thread_run_time_ms = 0;

            // Initialize the buffer
            auto *const key_buffer =
                    common::AllocationUtil::AllocateAligned(
                            default_index->GetProjectedRowInitializer().ProjectedRowSize());
            auto *const insert_key = default_index->GetProjectedRowInitializer().InitializeRow(
                    key_buffer);
            uint32_t my_num_inserts = num_inserts / num_threads;
            if (worker_id < num_inserts - my_num_inserts * num_threads)
                my_num_inserts++;
            auto *const txn = txn_manager_.BeginTransaction();

            // enumerate the tables, scan and create index
            for (int table_cnt = 0;
                 table_cnt * num_inserts_per_table_ < my_num_inserts; table_cnt++) {
                int table_index = table_cnt * num_threads + worker_id;
                storage::SqlTable *sql_table = sql_tables_[table_index];
                int num_to_insert = my_num_inserts - table_cnt * num_inserts_per_table_;
                if (num_to_insert > (int) num_inserts_per_table_)
                    num_to_insert = (int) num_inserts_per_table_;
                int num_inserted = 0;
                auto it = sql_table->begin();

                std::vector<catalog::col_oid_t> col_oids_for_use;
                col_oids_for_use.clear();
                for (int i = 0; i < num_columns; i++)
                    col_oids_for_use.push_back(col_oids_[i]);
                int num_to_scan;
                if (scan_all_)
                    num_to_scan = num_to_insert;
                else
                    num_to_scan = scan_size_kb_ / num_columns / 8;

                storage::ProjectedColumnsInitializer initializer = sql_table->InitializerForProjectedColumns(
                        col_oids_for_use, (uint32_t) num_to_scan);
                auto *buffer = common::AllocationUtil::AllocateAligned(
                        initializer.ProjectedColumnsSize());
                storage::ProjectedColumns *columns = initializer.Initialize(buffer);
                do {
                    sql_table->Scan(txn, &it, columns);
                    uint32_t num_read = columns->NumTuples();
                    double run_time_ms = 0;
                    {
                        // Time for index insertion is collected within this scope
                        execution::util::ScopedTimer<std::milli> timer(&run_time_ms);
                        for (uint32_t i = 0; i < num_read; i++) {
                            storage::ProjectedColumns::RowView stored = columns->InterpretAsRow(
                                    i);
                            for (uint16_t j = 0; j < (uint16_t) num_columns; j++)
                                *reinterpret_cast<int64_t *>(insert_key->AccessForceNotNull(
                                        j)) =
                                        *reinterpret_cast<int64_t *>(stored.AccessForceNotNull(
                                                j));
                            default_index->Insert(txn, *insert_key, columns->TupleSlots()[i]);
                            ++num_inserted;
                            if (num_inserted >= num_to_insert)
                                break;
                        }
                    }
                    thread_run_time_ms += run_time_ms;
                } while (num_inserted < num_to_insert && it != sql_table->end());
                delete[] buffer;
            }
            txn_manager_.Commit(txn, transaction::TransactionUtil::EmptyCallback, nullptr);
            *insert_time_ms = thread_run_time_ms;

            delete[] key_buffer;
        }

        /*
         * function of index insertion for each thread
         */
        void TableScan(uint32_t table_id, uint32_t num_scan, int num_columns) {
            auto *const txn = txn_manager_.BeginTransaction();
            storage::SqlTable *sql_table = sql_tables_[table_id];
            auto it = sql_table->begin();
            std::vector<catalog::col_oid_t> col_oids_for_use;
            col_oids_for_use.clear();
            for (int i = 0; i < num_columns; i++)
                col_oids_for_use.push_back(col_oids_[i]);
            storage::ProjectedColumnsInitializer initializer = sql_table->InitializerForProjectedColumns(
                    col_oids_for_use, num_scan_once_);
            auto *buffer = common::AllocationUtil::AllocateAligned(
                    initializer.ProjectedColumnsSize());
            storage::ProjectedColumns *columns = initializer.Initialize(buffer);
            uint32_t num_scanned = 0;
            do {
                sql_table->Scan(txn, &it, columns);
                num_scanned += num_scan_once_;
            } while (num_scanned < num_scan && it != sql_table->end());
            txn_manager_.Commit(txn, transaction::TransactionUtil::EmptyCallback, nullptr);
        }

        /*
         * Run a bunch of benchmark
         */
        void RunBenchmark() {
            // Initialize the time recorders for TPCH workload
            std::vector < std::vector<double> > interp_exec_ms_sum_single(tpch_filenum_);
            std::vector < std::vector<double> > adaptive_exec_ms_sum_single(tpch_filenum_);
            std::vector < std::vector<double> > jit_exec_ms_sum_single(tpch_filenum_);
            std::vector < std::vector<double> > cpu_time_ms_sum_single(tpch_filenum_);
            std::vector < std::vector<uint64_t> > interp_exec_ms_cnt_single(tpch_filenum_);
            std::vector < std::vector<uint64_t> > adaptive_exec_ms_cnt_single(tpch_filenum_);
            std::vector < std::vector<uint64_t> > jit_exec_ms_cnt_single(tpch_filenum_);
            std::vector < std::vector<uint64_t> > cpu_time_ms_cnt_single(tpch_filenum_);

            for (uint32_t j = 0; j < tpch_filenum_; j++) {
                interp_exec_ms_sum_single[j].resize(max_num_threads_);
                adaptive_exec_ms_sum_single[j].resize(max_num_threads_);
                jit_exec_ms_sum_single[j].resize(max_num_threads_);
                cpu_time_ms_sum_single[j].resize(max_num_threads_);
                interp_exec_ms_cnt_single[j].resize(max_num_threads_);
                adaptive_exec_ms_cnt_single[j].resize(max_num_threads_);
                jit_exec_ms_cnt_single[j].resize(max_num_threads_);
                cpu_time_ms_cnt_single[j].resize(max_num_threads_);
            }

            // Initialize the arrays of each thread for ARRAY workload
            std::vector <int> my_arrays[max_num_threads_];
            if (other_type_ == ARRAY or other_type_ == ARRAY10M)
                for (uint32_t i = 0; i < max_num_threads_; i++) {
                    my_arrays[i].resize(big_number_for_array_test_);
                    for (int j = 0; j < big_number_for_array_test_; j++)
                        my_arrays[i][j] = 1;
                }

            // If use perf, pause before running the experiment
            if (use_perf_) {
                std::cout << "Ready, press Enter to continue" << std::endl;
                std::getchar();
            }

            for (uint32_t num_inserts : num_inserts_list_)
                for (int num_columns : num_columns_list_)
                    for (uint32_t num_threads : num_threads_list_) {
                        // Initialize the time
                        double sum_time = 0;
                        double sum_insert_time = 0;
                        double sum_cpu_time = 0;
                        double insert_time_ms[max_num_threads_];

                        for (uint32_t i = 0; i < max_num_threads_; i++)
                            for (uint32_t j = 0; j < tpch_filenum_; j++) {
                                interp_exec_ms_sum_single[j][i] = 0;
                                adaptive_exec_ms_sum_single[j][i] = 0;
                                jit_exec_ms_sum_single[j][i] = 0;
                                cpu_time_ms_sum_single[j][i] = 0;
                                interp_exec_ms_cnt_single[j][i] = 0;
                                adaptive_exec_ms_cnt_single[j][i] = 0;
                                jit_exec_ms_cnt_single[j][i] = 0;
                                cpu_time_ms_cnt_single[j][i] = 0;
                            }

                        // Repeat the experiments for several times
                        for (int times = 1; times <= max_times_; times++) {
                            storage::index::Index *default_index;
                            if (workload_type_ == UINDEX) {
                                default_index = IndexInit(num_columns);
                                gc_thread_->GetGarbageCollector().RegisterIndexForGC(default_index);
                            }

                            common::WorkerPool workload_thread_pool{num_threads, {}};
                            common::WorkerPool other_thread_pool{max_num_threads_ - num_threads, {}};
                            std::atomic <bool> unfinished = true;

                            // Workload for LOOP, ARRAY, ARRAY10M, TPCH or SCAN
                            auto run_other = [&](uint32_t worker_id, uint32_t core_id) {
                                if (pin_to_core_)
                                    MyPinToCore(core_id);
                                switch (other_type_) {
                                    case EMPTY:
                                        break;
                                    case LOOP:
                                        LoopFunction(&unfinished);
                                        break;
                                    case ARRAY:
                                        ArrayFunction(&unfinished, my_arrays[worker_id], big_number_for_array_test_);
                                        break;
                                    case ARRAY10M:
                                        // the total size fo arrays is 10M
                                        ArrayFunction(&unfinished, my_arrays[worker_id], big_number_for_array_test_ /
                                                                                         (max_num_threads_ - num_threads));
                                        break;
                                    case INDEX:
                                        while(unfinished) {
                                            storage::index::Index * my_index = IndexInit(num_columns);
                                            gc_thread_->GetGarbageCollector().RegisterIndexForGC(my_index);
                                            IndexInsertion(worker_id, my_index, num_inserts, num_columns, 1, &insert_time_ms[worker_id]);
                                            gc_thread_->GetGarbageCollector().UnregisterIndexForGC(my_index);
                                            delete my_index;
                                        }
                                        break;
                                    default:
                                        execution::TplClass my_tpch(&txn_manager_, &sample_output_, db_oid_,
                                                                    *catalog_pointer_, &unfinished);
                                        // useless variables
                                        double x1 = 0, x2 = 0, x3 = 0, x4 = 0;
                                        uint64_t y1 = 0, y2 = 0, y3 = 0, y4 = 0;
                                        if (other_type_ == TPCH) {
                                            for (int fn = worker_id % tpch_filenum_; unfinished; fn = (fn + 1) %
                                                                                                      tpch_filenum_) {
                                                my_tpch.RunFile(tpch_filename_[fn],
                                                                &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4,
                                                                tpch_mode_[0], tpch_mode_[1], tpch_mode_[2]);
                                            }
                                        } else {
                                            while (unfinished)
                                                my_tpch.RunFile(scan_filename_,
                                                                &x1, &y1, &x2, &y2, &x3, &y3, &x4, &y4,
                                                                tpch_mode_[0], tpch_mode_[1], tpch_mode_[2]);
                                        }
                                        break;
                                }
                            };

                            // Index creation workload
                            auto workload = [&](uint32_t worker_id, uint32_t core_id) {
                                if (pin_to_core_)
                                    MyPinToCore(core_id);
                                switch (workload_type_) {
                                    case UINDEX:
                                        IndexInsertion(worker_id, default_index, num_inserts, num_columns, num_threads,
                                                       &insert_time_ms[worker_id]);
                                        break;
                                    case UTPCH: {
                                        execution::TplClass my_tpch(&txn_manager_, &sample_output_benchmark_,
                                                                    db_oid_benchmark_,
                                                                    *catalog_pointer_, &unfinished);
                                        for (int i = 0; i < tpch_repeated_times_; i++) {
                                            my_tpch.RunFile(tpch_filename_[tpch_number_],
                                                            &interp_exec_ms_sum_single[tpch_number_][worker_id],
                                                            &interp_exec_ms_cnt_single[tpch_number_][worker_id],
                                                            &adaptive_exec_ms_sum_single[tpch_number_][worker_id],
                                                            &adaptive_exec_ms_cnt_single[tpch_number_][worker_id],
                                                            &jit_exec_ms_sum_single[tpch_number_][worker_id],
                                                            &jit_exec_ms_cnt_single[tpch_number_][worker_id],
                                                            &cpu_time_ms_sum_single[tpch_number_][worker_id],
                                                            &cpu_time_ms_cnt_single[tpch_number_][worker_id],
                                                            tpch_mode_[0], tpch_mode_[1], tpch_mode_[2]);
                                        }
                                    }
                                        break;
                                    case USCAN: {
                                        execution::TplClass my_tpch(&txn_manager_, &sample_output_benchmark_,
                                                                    db_oid_benchmark_,
                                                                    *catalog_pointer_, &unfinished);
                                        // record time in interp_exec_ms_sum_single[0] and so on...
                                        for (int i = 0; i < tpch_repeated_times_; i++) {
                                            my_tpch.RunFile(scan_filename_,
                                                            &interp_exec_ms_sum_single[0][worker_id],
                                                            &interp_exec_ms_cnt_single[0][worker_id],
                                                            &adaptive_exec_ms_sum_single[0][worker_id],
                                                            &adaptive_exec_ms_cnt_single[0][worker_id],
                                                            &jit_exec_ms_sum_single[0][worker_id],
                                                            &jit_exec_ms_cnt_single[0][worker_id],
                                                            &cpu_time_ms_sum_single[0][worker_id],
                                                            &cpu_time_ms_cnt_single[0][worker_id],
                                                            tpch_mode_[0], tpch_mode_[1], tpch_mode_[2]);
                                        }
                                    }
                                        break;
                                    case ULOOP: {
                                        std::atomic<bool> always_false = false;
                                        LoopFunction(&always_false);
                                    }
                                        break;
                                    case UTABLE:
                                        TableScan(worker_id, num_inserts_per_table_, max_num_columns_);
                                        break;
                                }
                            };

                            // Workloads except index creation
                            if (one_always_) { // always have some workload on core 20
                                uint32_t i = 0;
                                uint32_t core_id = fixed_core_id_;
                                other_thread_pool.SubmitTask([i, core_id, &run_other] { run_other(i, core_id); });
                            } else if (other_type_ != EMPTY) {
                                for (uint32_t i = 0; i < max_num_threads_ - num_threads; i++) {
                                    uint32_t core_id = core_ids_[i + num_threads];
                                    other_thread_pool.SubmitTask(
                                            [i, core_id, &run_other] { run_other(i, core_id); });
                                }
                            }

                            double elapsed_ms, cpu_time_ms;
                            {
                                // Index creation workloads
                                common::ThreadCPUTimer cpu_timer;
                                execution::util::ScopedTimer<std::milli> timer(&elapsed_ms);
                                cpu_timer.Start();
                                // run the workload
                                for (uint32_t i = 0; i < num_threads; i++) {
                                    uint32_t core_id = core_ids_[i];
                                    workload_thread_pool.SubmitTask([i, core_id, &workload] { workload(i, core_id); });
                                }
                                workload_thread_pool.WaitUntilAllFinished();
                                cpu_timer.Stop();
                                cpu_time_ms = (double)cpu_timer.ElapsedTime().user_time_us_ / 1000.0;
                            }
                            unfinished = false;
                            other_thread_pool.WaitUntilAllFinished();

                            if (workload_type_ == UINDEX) {
                                gc_thread_->GetGarbageCollector().UnregisterIndexForGC(default_index);
                                delete default_index;
                            }

                            // Record the time
                            sum_time += elapsed_ms;
                            double max_insert_time = 0;
                            for (uint32_t i = 0; i < num_threads; i++)
                                if (insert_time_ms[i] > max_insert_time)
                                    max_insert_time = insert_time_ms[i];
                            sum_insert_time += max_insert_time;
                            sum_cpu_time += cpu_time_ms;
                        }

                        // output format: keysize, threadnum, inertnum, time including scan, time without scan (split by \t)
                        if (workload_type_ == UINDEX || workload_type_ == ULOOP || workload_type_ == UTABLE) {
                            std::cout << "bwtree_time" << "\t" << num_columns << "\t" << num_threads << "\t"
                                      << num_inserts << "\t" << sum_time / max_times_ / 1000.0
                                      << "\t" << sum_insert_time / max_times_ / 1000.0
                                      << "\t" << sum_cpu_time / max_times_ / 1000.0 << std::endl;
                        }

                        // Compute the time of TPCH workloads
                        double interp_exec_ms_sum[tpch_filenum_], adaptive_exec_ms_sum[tpch_filenum_], jit_exec_ms_sum[tpch_filenum_], cpu_time_ms_sum[tpch_filenum_];
                        uint64_t interp_exec_ms_cnt[tpch_filenum_], adaptive_exec_ms_cnt[tpch_filenum_], jit_exec_ms_cnt[tpch_filenum_], cpu_time_ms_cnt[tpch_filenum_];
                        for (uint32_t j = 0; j < tpch_filenum_; j++) {
                            interp_exec_ms_sum[j] = 0;
                            interp_exec_ms_cnt[j] = 0;
                            adaptive_exec_ms_sum[j] = 0;
                            adaptive_exec_ms_cnt[j] = 0;
                            jit_exec_ms_sum[j] = 0;
                            jit_exec_ms_cnt[j] = 0;
                            cpu_time_ms_sum[j] = 0;
                            cpu_time_ms_cnt[j] = 0;
                        }
                        for (uint32_t i = 0; i < max_num_threads_; i++)
                            for (uint32_t j = 0; j < tpch_filenum_; j++) {
                                interp_exec_ms_sum[j] += interp_exec_ms_sum_single[j][i];
                                interp_exec_ms_cnt[j] += interp_exec_ms_cnt_single[j][i];
                                adaptive_exec_ms_sum[j] += adaptive_exec_ms_sum_single[j][i];
                                adaptive_exec_ms_cnt[j] += adaptive_exec_ms_cnt_single[j][i];
                                jit_exec_ms_sum[j] += jit_exec_ms_sum_single[j][i];
                                jit_exec_ms_cnt[j] += jit_exec_ms_cnt_single[j][i];
                                cpu_time_ms_sum[j] += cpu_time_ms_sum_single[j][i];
                                cpu_time_ms_cnt[j] += cpu_time_ms_cnt_single[j][i];
                            }

                        // output format: filename, keysize, threadnum, insertnum, interp_time, adaptive_time, jit_time(ms) (split by \t)
                        for (uint32_t j = 0; j < tpch_filenum_; j++)
                            if (interp_exec_ms_cnt[j] > 0 || adaptive_exec_ms_cnt[j] > 0 || jit_exec_ms_cnt[j] > 0) {
                                std::cout << tpch_filename_[j] << "\t" << num_columns << "\t" << num_threads
                                          << "\t"
                                          << num_inserts << "\t" << j
                                          << "\t" << interp_exec_ms_sum[j] / (double)interp_exec_ms_cnt[j]
                                          << "\t" << adaptive_exec_ms_sum[j] / (double)adaptive_exec_ms_cnt[j]
                                          << "\t" << jit_exec_ms_sum[j] / (double)jit_exec_ms_cnt[j]
                                          << "\t" << cpu_time_ms_sum[j] / (double)cpu_time_ms_cnt[j]
                                          << std::endl;
                            }
                    }
            // If use perf, pause before TearDown
            if (use_perf_) {
                std::cout << "Finished, press Enter to continue" << std::endl;
                std::getchar();
            }
        }
    };

    /*
     * benchmark for index creation with random data, or TPCH benchmark
     */
    BENCHMARK_DEFINE_F(IndexBenchmark, RandomInsert)(benchmark::State &state) {
    for (auto _ : state) {
        switch (workload_type_) {
        case UINDEX:
        case ULOOP:
        case USCAN:
        case UTABLE:
            RunBenchmark();
        break;
        case UTPCH:
            if (single_test_) {
                for (; max_num_threads_ >= 1; max_num_threads_--) {
                    std::cout << max_num_threads_ << '\t';
                    RunBenchmark();
                }
                break;
            }
            std::cout << "Empty" << std::endl;
            other_type_ = EMPTY;
            one_always_ = false;
            for (int tpch_number : tpch_list_) {
                tpch_number_ = tpch_number;
                RunBenchmark();
            }

            for (other_types other_type : {LOOP, TPCH, SCAN, INDEX}) {
                other_type_ = other_type;
                std::cout << "Hyper-threading " << type_names_[int(other_type)] << std::endl;
                one_always_ = true;
                for (int tpch_number : tpch_list_) {
                    tpch_number_ = tpch_number;
                    RunBenchmark();
                }

                std::cout << "Other physical cores " << type_names_[int(other_type)] << std::endl;
                one_always_ = false;
                if (local_test_)
                    max_num_threads_ = 3;
                else
                    max_num_threads_ = 17;
                for (int tpch_number : tpch_list_) {
                    tpch_number_ = tpch_number;
                    RunBenchmark();
                }

                std::cout << "All other cores " << type_names_[int(other_type)] << std::endl;
                max_num_threads_++;
                for (int tpch_number : tpch_list_) {
                    tpch_number_ = tpch_number;
                    RunBenchmark();
                }
            }
            break;
        }
    }
    if (local_test_)
        max_num_threads_ = 4;
    else
        max_num_threads_ = 18; // to delete all the tables
}

BENCHMARK_REGISTER_F(IndexBenchmark, RandomInsert)
->Unit(benchmark::kMillisecond)
->MinTime(1);
}  // namespace terrier