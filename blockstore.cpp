#include "blockstore.h"

blockstore::blockstore(spp::sparse_hash_map<std::string, std::string> & config, ring_loop_t *ringloop)
{
    this->ringloop = ringloop;
    ring_consumer.handle_event = [this](ring_data_t *d) { handle_event(d); };
    ring_consumer.loop = [this]() { loop(); };
    ringloop->register_consumer(ring_consumer);
    initialized = 0;
    block_order = stoull(config["block_size_order"]);
    block_size = 1 << block_order;
    if (block_size <= 1 || block_size >= MAX_BLOCK_SIZE)
    {
        throw new std::runtime_error("Bad block size");
    }
    data_fd = meta_fd = journal.fd = -1;
    try
    {
        open_data(config);
        open_meta(config);
        open_journal(config);
        calc_lengths(config);
        data_alloc = allocator_create(block_count);
        if (!data_alloc)
            throw new std::bad_alloc();
    }
    catch (std::exception & e)
    {
        if (data_fd >= 0)
            close(data_fd);
        if (meta_fd >= 0 && meta_fd != data_fd)
            close(meta_fd);
        if (journal.fd >= 0 && journal.fd != meta_fd)
            close(journal.fd);
        throw e;
    }
}

blockstore::~blockstore()
{
    ringloop->unregister_consumer(ring_consumer.number);
    if (data_fd >= 0)
        close(data_fd);
    if (meta_fd >= 0 && meta_fd != data_fd)
        close(meta_fd);
    if (journal.fd >= 0 && journal.fd != meta_fd)
        close(journal.fd);
    free(journal.sector_buf);
    free(journal.sector_info);
}

// main event loop - handle requests
void blockstore::handle_event(ring_data_t *data)
{
    if (initialized != 10)
    {
        if (metadata_init_reader)
        {
            metadata_init_reader->handle_event(data);
        }
        else if (journal_init_reader)
        {
            journal_init_reader->handle_event(data);
        }
    }
    else
    {
        struct blockstore_operation* op = (struct blockstore_operation*)data->op;
        if ((op->flags & OP_TYPE_MASK) == OP_READ_DIRTY ||
            (op->flags & OP_TYPE_MASK) == OP_READ)
        {
            op->pending_ops--;
            if (data->res < 0)
            {
                // read error
                op->retval = data->res;
            }
            if (op->pending_ops == 0)
            {
                if (op->retval == 0)
                    op->retval = op->len;
                op->callback(op);
                in_process_ops.erase(op);
            }
        }
        else if ((op->flags & OP_TYPE_MASK) == OP_WRITE ||
            (op->flags & OP_TYPE_MASK) == OP_DELETE)
        {
            op->pending_ops--;
            if (data->res < 0)
            {
                // write error
                // FIXME: our state becomes corrupted after a write error. maybe do something better than just die
                throw new std::runtime_error("write operation failed. in-memory state is corrupted. AAAAAAAaaaaaaaaa!!!111");
            }
            if (op->used_journal_sector > 0)
            {
                uint64_t s = op->used_journal_sector-1;
                if (journal.sector_info[s].usage_count > 0)
                {
                    // The last write to this journal sector was made by this op, release the buffer
                    journal.sector_info[s].usage_count--;
                }
                op->used_journal_sector = 0;
            }
            if (op->pending_ops == 0)
            {
                // Acknowledge write without sync
                auto dirty_it = dirty_db.find((obj_ver_id){
                    .oid = op->oid,
                    .version = op->version,
                });
                dirty_it->second.state = (dirty_it->second.state == ST_J_SUBMITTED
                    ? ST_J_WRITTEN : (dirty_it->second.state == ST_DEL_SUBMITTED ? ST_DEL_WRITTEN : ST_D_WRITTEN));
                op->retval = op->len;
                op->callback(op);
                in_process_ops.erase(op);
                unsynced_writes.push_back((obj_ver_id){
                    .oid = op->oid,
                    .version = op->version,
                });
            }
        }
        else if ((op->flags & OP_TYPE_MASK) == OP_SYNC)
        {
            
        }
        else if ((op->flags & OP_TYPE_MASK) == OP_STABLE)
        {
            
        }
    }
}

// main event loop - produce requests
void blockstore::loop()
{
    if (initialized != 10)
    {
        // read metadata, then journal
        if (initialized == 0)
        {
            metadata_init_reader = new blockstore_init_meta(this);
            initialized = 1;
        }
        else if (initialized == 1)
        {
            int res = metadata_init_reader->loop();
            if (!res)
            {
                delete metadata_init_reader;
                metadata_init_reader = NULL;
                journal_init_reader = new blockstore_init_journal(this);
                initialized = 2;
            }
        }
        else if (initialized == 2)
        {
            int res = journal_init_reader->loop();
            if (!res)
            {
                delete journal_init_reader;
                journal_init_reader = NULL;
                initialized = 10;
            }
        }
    }
    else
    {
        // try to submit ops
        auto cur = submit_queue.begin();
        bool has_writes = false;
        while (cur != submit_queue.end())
        {
            auto op_ptr = cur;
            auto op = *(cur++);
            if (op->wait_for)
            {
                if (op->wait_for == WAIT_SQE)
                {
                    if (io_uring_sq_space_left(ringloop->ring) < op->wait_detail)
                    {
                        // stop submission if there's still no free space
                        break;
                    }
                    op->wait_for = 0;
                }
                else if (op->wait_for == WAIT_IN_FLIGHT)
                {
                    auto dirty_it = dirty_db.find((obj_ver_id){
                        .oid = op->oid,
                        .version = op->wait_detail,
                    });
                    if (dirty_it != dirty_db.end() && IS_IN_FLIGHT(dirty_it->second.state))
                    {
                        // do not submit
                        continue;
                    }
                    op->wait_for = 0;
                }
                else if (op->wait_for == WAIT_JOURNAL)
                {
                    if (journal.used_start < op->wait_detail)
                    {
                        // do not submit
                        continue;
                    }
                    op->wait_for = 0;
                }
                else if (op->wait_for == WAIT_JOURNAL_BUFFER)
                {
                    if (journal.sector_info[((journal.cur_sector + 1) % journal.sector_count)].usage_count > 0)
                    {
                        // do not submit
                        continue;
                    }
                    op->wait_for = 0;
                }
                else
                {
                    throw new std::runtime_error("BUG: op->wait_for value is unexpected");
                }
            }
            if ((op->flags & OP_TYPE_MASK) == OP_READ_DIRTY ||
                (op->flags & OP_TYPE_MASK) == OP_READ)
            {
                int dequeue_op = dequeue_read(op);
                if (dequeue_op)
                {
                    submit_queue.erase(op_ptr);
                }
                else if (op->wait_for == WAIT_SQE)
                {
                    // ring is full, stop submission
                    break;
                }
            }
            else if ((op->flags & OP_TYPE_MASK) == OP_WRITE ||
                (op->flags & OP_TYPE_MASK) == OP_DELETE)
            {
                int dequeue_op = dequeue_write(op);
                if (dequeue_op)
                {
                    submit_queue.erase(op_ptr);
                }
                else if (op->wait_for == WAIT_SQE)
                {
                    // ring is full, stop submission
                    break;
                }
                has_writes = true;
            }
            else if ((op->flags & OP_TYPE_MASK) == OP_SYNC)
            {
                // wait for all small writes to be submitted
                // wait for all big writes to complete, submit data device fsync
                // wait for the data device fsync to complete, then submit journal writes for big writes
                // then submit an fsync operation
                if (has_writes)
                {
                    // Can't submit SYNC before previous writes
                    continue;
                }
                int dequeue_op = dequeue_sync(op);
                if (dequeue_op)
                {
                    submit_queue.erase(op_ptr);
                }
                else if (op->wait_for == WAIT_SQE)
                {
                    // ring is full, stop submission
                    break;
                }
            }
            else if ((op->flags & OP_TYPE_MASK) == OP_STABLE)
            {
                
            }
        }
    }
}

int blockstore::enqueue_op(blockstore_operation *op)
{
    if (op->offset >= block_size || op->len >= block_size-op->offset ||
        (op->len % DISK_ALIGNMENT) ||
        (op->flags & OP_TYPE_MASK) < OP_READ || (op->flags & OP_TYPE_MASK) > OP_DELETE)
    {
        // Basic verification not passed
        return -EINVAL;
    }
    op->wait_for = 0;
    submit_queue.push_back(op);
    if ((op->flags & OP_TYPE_MASK) == OP_WRITE)
    {
        // Assign version number
        auto dirty_it = dirty_db.upper_bound((obj_ver_id){
            .oid = op->oid,
            .version = UINT64_MAX,
        });
        dirty_it--;
        if (dirty_it != dirty_db.end() && dirty_it->first.oid == op->oid)
        {
            op->version = dirty_it->first.version + 1;
        }
        else
        {
            auto clean_it = object_db.find(op->oid);
            if (clean_it != object_db.end())
            {
                op->version = clean_it->second.version + 1;
            }
            else
            {
                op->version = 1;
            }
        }
        // Immediately add the operation into dirty_db, so subsequent reads could see it
        dirty_db.emplace((obj_ver_id){
            .oid = op->oid,
            .version = op->version,
        }, (dirty_entry){
            .state = ST_IN_FLIGHT,
            .flags = 0,
            .location = 0,
            .offset = op->offset,
            .size = op->len,
        });
    }
    return 0;
}