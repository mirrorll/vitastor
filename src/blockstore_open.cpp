// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 (see README.md for details)

#include <sys/file.h>
#include "blockstore_impl.h"

static uint32_t is_power_of_two(uint64_t value)
{
    uint32_t l = 0;
    while (value > 1)
    {
        if (value & 1)
        {
            return 64;
        }
        value = value >> 1;
        l++;
    }
    return l;
}

void blockstore_impl_t::parse_config(blockstore_config_t & config)
{
    // Parse
    if (config["readonly"] == "true" || config["readonly"] == "1" || config["readonly"] == "yes")
    {
        readonly = true;
    }
    if (config["disable_data_fsync"] == "true" || config["disable_data_fsync"] == "1" || config["disable_data_fsync"] == "yes")
    {
        disable_data_fsync = true;
    }
    if (config["disable_meta_fsync"] == "true" || config["disable_meta_fsync"] == "1" || config["disable_meta_fsync"] == "yes")
    {
        disable_meta_fsync = true;
    }
    if (config["disable_journal_fsync"] == "true" || config["disable_journal_fsync"] == "1" || config["disable_journal_fsync"] == "yes")
    {
        disable_journal_fsync = true;
    }
    if (config["disable_device_lock"] == "true" || config["disable_device_lock"] == "1" || config["disable_device_lock"] == "yes")
    {
        disable_flock = true;
    }
    if (config["flush_journal"] == "true" || config["flush_journal"] == "1" || config["flush_journal"] == "yes")
    {
        // Only flush journal and exit
        journal.flush_journal = true;
    }
    if (config["immediate_commit"] == "all")
    {
        immediate_commit = IMMEDIATE_ALL;
    }
    else if (config["immediate_commit"] == "small")
    {
        immediate_commit = IMMEDIATE_SMALL;
    }
    metadata_buf_size = strtoull(config["meta_buf_size"].c_str(), NULL, 10);
    cfg_journal_size = strtoull(config["journal_size"].c_str(), NULL, 10);
    data_device = config["data_device"];
    data_offset = strtoull(config["data_offset"].c_str(), NULL, 10);
    cfg_data_size = strtoull(config["data_size"].c_str(), NULL, 10);
    meta_device = config["meta_device"];
    meta_offset = strtoull(config["meta_offset"].c_str(), NULL, 10);
    block_size = strtoull(config["block_size"].c_str(), NULL, 10);
    inmemory_meta = config["inmemory_metadata"] != "false";
    journal_device = config["journal_device"];
    journal.offset = strtoull(config["journal_offset"].c_str(), NULL, 10);
    journal.sector_count = strtoull(config["journal_sector_buffer_count"].c_str(), NULL, 10);
    journal.no_same_sector_overwrites = config["journal_no_same_sector_overwrites"] == "true" ||
        config["journal_no_same_sector_overwrites"] == "1" || config["journal_no_same_sector_overwrites"] == "yes";
    journal.inmemory = config["inmemory_journal"] != "false";
    disk_alignment = strtoull(config["disk_alignment"].c_str(), NULL, 10);
    journal_block_size = strtoull(config["journal_block_size"].c_str(), NULL, 10);
    meta_block_size = strtoull(config["meta_block_size"].c_str(), NULL, 10);
    bitmap_granularity = strtoull(config["bitmap_granularity"].c_str(), NULL, 10);
    max_flusher_count = strtoull(config["max_flusher_count"].c_str(), NULL, 10);
    if (!max_flusher_count)
        max_flusher_count = strtoull(config["flusher_count"].c_str(), NULL, 10);
    min_flusher_count = strtoull(config["min_flusher_count"].c_str(), NULL, 10);
    max_write_iodepth = strtoull(config["max_write_iodepth"].c_str(), NULL, 10);
    throttle_small_writes = config["throttle_small_writes"] == "true" || config["throttle_small_writes"] == "1" || config["throttle_small_writes"] == "yes";
    throttle_target_iops = strtoull(config["throttle_target_iops"].c_str(), NULL, 10);
    throttle_target_mbs = strtoull(config["throttle_target_mbs"].c_str(), NULL, 10);
    throttle_target_parallelism = strtoull(config["throttle_target_parallelism"].c_str(), NULL, 10);
    throttle_threshold_us = strtoull(config["throttle_threshold_us"].c_str(), NULL, 10);
    // Validate
    if (!block_size)
    {
        block_size = (1 << DEFAULT_ORDER);
    }
    if ((block_order = is_power_of_two(block_size)) >= 64 || block_size < MIN_BLOCK_SIZE || block_size >= MAX_BLOCK_SIZE)
    {
        throw std::runtime_error("Bad block size");
    }
    if (!max_flusher_count)
    {
        max_flusher_count = 256;
    }
    if (!min_flusher_count || journal.flush_journal)
    {
        min_flusher_count = 1;
    }
    if (!max_write_iodepth)
    {
        max_write_iodepth = 128;
    }
    if (!disk_alignment)
    {
        disk_alignment = 4096;
    }
    else if (disk_alignment % MEM_ALIGNMENT)
    {
        throw std::runtime_error("disk_alignment must be a multiple of "+std::to_string(MEM_ALIGNMENT));
    }
    if (!journal_block_size)
    {
        journal_block_size = 4096;
    }
    else if (journal_block_size % MEM_ALIGNMENT)
    {
        throw std::runtime_error("journal_block_size must be a multiple of "+std::to_string(MEM_ALIGNMENT));
    }
    if (!meta_block_size)
    {
        meta_block_size = 4096;
    }
    else if (meta_block_size % MEM_ALIGNMENT)
    {
        throw std::runtime_error("meta_block_size must be a multiple of "+std::to_string(MEM_ALIGNMENT));
    }
    if (data_offset % disk_alignment)
    {
        throw std::runtime_error("data_offset must be a multiple of disk_alignment = "+std::to_string(disk_alignment));
    }
    if (!bitmap_granularity)
    {
        bitmap_granularity = DEFAULT_BITMAP_GRANULARITY;
    }
    else if (bitmap_granularity % disk_alignment)
    {
        throw std::runtime_error("Sparse write tracking granularity must be a multiple of disk_alignment = "+std::to_string(disk_alignment));
    }
    if (block_size % bitmap_granularity)
    {
        throw std::runtime_error("Block size must be a multiple of sparse write tracking granularity");
    }
    if (journal_device == meta_device || meta_device == "" && journal_device == data_device)
    {
        journal_device = "";
    }
    if (meta_device == data_device)
    {
        meta_device = "";
    }
    if (meta_offset % meta_block_size)
    {
        throw std::runtime_error("meta_offset must be a multiple of meta_block_size = "+std::to_string(meta_block_size));
    }
    if (journal.offset % journal_block_size)
    {
        throw std::runtime_error("journal_offset must be a multiple of journal_block_size = "+std::to_string(journal_block_size));
    }
    if (journal.sector_count < 2)
    {
        journal.sector_count = 32;
    }
    if (metadata_buf_size < 65536)
    {
        metadata_buf_size = 4*1024*1024;
    }
    if (meta_device == "")
    {
        disable_meta_fsync = disable_data_fsync;
    }
    if (journal_device == "")
    {
        disable_journal_fsync = disable_meta_fsync;
    }
    if (immediate_commit != IMMEDIATE_NONE && !disable_journal_fsync)
    {
        throw std::runtime_error("immediate_commit requires disable_journal_fsync");
    }
    if (immediate_commit == IMMEDIATE_ALL && !disable_data_fsync)
    {
        throw std::runtime_error("immediate_commit=all requires disable_journal_fsync and disable_data_fsync");
    }
    if (!throttle_target_iops)
    {
        throttle_target_iops = 100;
    }
    if (!throttle_target_mbs)
    {
        throttle_target_mbs = 100;
    }
    if (!throttle_target_parallelism)
    {
        throttle_target_parallelism = 1;
    }
    if (!throttle_threshold_us)
    {
        throttle_threshold_us = 50;
    }
    // init some fields
    clean_entry_bitmap_size = block_size / bitmap_granularity / 8;
    clean_entry_size = sizeof(clean_disk_entry) + 2*clean_entry_bitmap_size;
    journal.block_size = journal_block_size;
    journal.next_free = journal_block_size;
    journal.used_start = journal_block_size;
    // no free space because sector is initially unmapped
    journal.in_sector_pos = journal_block_size;
}

void blockstore_impl_t::calc_lengths()
{
    // data
    data_len = data_size - data_offset;
    if (data_fd == meta_fd && data_offset < meta_offset)
    {
        data_len = meta_offset - data_offset;
    }
    if (data_fd == journal.fd && data_offset < journal.offset)
    {
        data_len = data_len < journal.offset-data_offset
            ? data_len : journal.offset-data_offset;
    }
    if (cfg_data_size != 0)
    {
        if (data_len < cfg_data_size)
        {
            throw std::runtime_error("Data area ("+std::to_string(data_len)+
                " bytes) is less than configured size ("+std::to_string(cfg_data_size)+" bytes)");
        }
        data_len = cfg_data_size;
    }
    // meta
    meta_area = (meta_fd == data_fd ? data_size : meta_size) - meta_offset;
    if (meta_fd == data_fd && meta_offset <= data_offset)
    {
        meta_area = data_offset - meta_offset;
    }
    if (meta_fd == journal.fd && meta_offset <= journal.offset)
    {
        meta_area = meta_area < journal.offset-meta_offset
            ? meta_area : journal.offset-meta_offset;
    }
    // journal
    journal.len = (journal.fd == data_fd ? data_size : (journal.fd == meta_fd ? meta_size : journal.device_size)) - journal.offset;
    if (journal.fd == data_fd && journal.offset <= data_offset)
    {
        journal.len = data_offset - journal.offset;
    }
    if (journal.fd == meta_fd && journal.offset <= meta_offset)
    {
        journal.len = journal.len < meta_offset-journal.offset
            ? journal.len : meta_offset-journal.offset;
    }
    // required metadata size
    block_count = data_len / block_size;
    meta_len = (1 + (block_count - 1 + meta_block_size / clean_entry_size) / (meta_block_size / clean_entry_size)) * meta_block_size;
    if (meta_area < meta_len)
    {
        throw std::runtime_error("Metadata area is too small, need at least "+std::to_string(meta_len)+" bytes");
    }
    if (inmemory_meta)
    {
        metadata_buffer = memalign(MEM_ALIGNMENT, meta_len);
        if (!metadata_buffer)
            throw std::runtime_error("Failed to allocate memory for the metadata");
    }
    else if (clean_entry_bitmap_size)
    {
        clean_bitmap = (uint8_t*)malloc(block_count * 2*clean_entry_bitmap_size);
        if (!clean_bitmap)
            throw std::runtime_error("Failed to allocate memory for the metadata sparse write bitmap");
    }
    // requested journal size
    if (cfg_journal_size > journal.len)
    {
        throw std::runtime_error("Requested journal_size is too large");
    }
    else if (cfg_journal_size > 0)
    {
        journal.len = cfg_journal_size;
    }
    if (journal.len < MIN_JOURNAL_SIZE)
    {
        throw std::runtime_error("Journal is too small, need at least "+std::to_string(MIN_JOURNAL_SIZE)+" bytes");
    }
    if (journal.inmemory)
    {
        journal.buffer = memalign(MEM_ALIGNMENT, journal.len);
        if (!journal.buffer)
            throw std::runtime_error("Failed to allocate memory for journal");
    }
}

static void check_size(int fd, uint64_t *size, uint64_t *sectsize, std::string name)
{
    int sect;
    struct stat st;
    if (fstat(fd, &st) < 0)
    {
        throw std::runtime_error("Failed to stat "+name);
    }
    if (S_ISREG(st.st_mode))
    {
        *size = st.st_size;
        if (sectsize)
        {
            *sectsize = st.st_blksize;
        }
    }
    else if (S_ISBLK(st.st_mode))
    {
        if (ioctl(fd, BLKGETSIZE64, size) < 0 ||
            ioctl(fd, BLKSSZGET, &sect) < 0)
        {
            throw std::runtime_error("failed to get "+name+" size or block size: "+strerror(errno));
        }
        if (sectsize)
        {
            *sectsize = sect;
        }
    }
    else
    {
        throw std::runtime_error(name+" is neither a file nor a block device");
    }
}

void blockstore_impl_t::open_data()
{
    data_fd = open(data_device.c_str(), O_DIRECT|O_RDWR);
    if (data_fd == -1)
    {
        throw std::runtime_error("Failed to open data device");
    }
    check_size(data_fd, &data_size, &data_device_sect, "data device");
    if (disk_alignment % data_device_sect)
    {
        throw std::runtime_error(
            "disk_alignment ("+std::to_string(disk_alignment)+
            ") is not a multiple of data device sector size ("+std::to_string(data_device_sect)+")"
        );
    }
    if (data_offset >= data_size)
    {
        throw std::runtime_error("data_offset exceeds device size = "+std::to_string(data_size));
    }
    if (!disable_flock && flock(data_fd, LOCK_EX|LOCK_NB) != 0)
    {
        throw std::runtime_error(std::string("Failed to lock data device: ") + strerror(errno));
    }
}

void blockstore_impl_t::open_meta()
{
    if (meta_device != "")
    {
        meta_offset = 0;
        meta_fd = open(meta_device.c_str(), O_DIRECT|O_RDWR);
        if (meta_fd == -1)
        {
            throw std::runtime_error("Failed to open metadata device");
        }
        check_size(meta_fd, &meta_size, &meta_device_sect, "metadata device");
        if (meta_offset >= meta_size)
        {
            throw std::runtime_error("meta_offset exceeds device size = "+std::to_string(meta_size));
        }
        if (!disable_flock && flock(meta_fd, LOCK_EX|LOCK_NB) != 0)
        {
            throw std::runtime_error(std::string("Failed to lock metadata device: ") + strerror(errno));
        }
    }
    else
    {
        meta_fd = data_fd;
        meta_device_sect = data_device_sect;
        meta_size = 0;
        if (meta_offset >= data_size)
        {
            throw std::runtime_error("meta_offset exceeds device size = "+std::to_string(data_size));
        }
    }
    if (meta_block_size % meta_device_sect)
    {
        throw std::runtime_error(
            "meta_block_size ("+std::to_string(meta_block_size)+
            ") is not a multiple of data device sector size ("+std::to_string(meta_device_sect)+")"
        );
    }
}

void blockstore_impl_t::open_journal()
{
    if (journal_device != "")
    {
        journal.fd = open(journal_device.c_str(), O_DIRECT|O_RDWR);
        if (journal.fd == -1)
        {
            throw std::runtime_error("Failed to open journal device");
        }
        check_size(journal.fd, &journal.device_size, &journal_device_sect, "journal device");
        if (!disable_flock && flock(journal.fd, LOCK_EX|LOCK_NB) != 0)
        {
            throw std::runtime_error(std::string("Failed to lock journal device: ") + strerror(errno));
        }
    }
    else
    {
        journal.fd = meta_fd;
        journal_device_sect = meta_device_sect;
        journal.device_size = 0;
        if (journal.offset >= data_size)
        {
            throw std::runtime_error("journal_offset exceeds device size");
        }
    }
    journal.sector_info = (journal_sector_info_t*)calloc(journal.sector_count, sizeof(journal_sector_info_t));
    if (!journal.sector_info)
    {
        throw std::bad_alloc();
    }
    if (!journal.inmemory)
    {
        journal.sector_buf = (uint8_t*)memalign(MEM_ALIGNMENT, journal.sector_count * journal_block_size);
        if (!journal.sector_buf)
            throw std::bad_alloc();
    }
    if (journal_block_size % journal_device_sect)
    {
        throw std::runtime_error(
            "journal_block_size ("+std::to_string(journal_block_size)+
            ") is not a multiple of journal device sector size ("+std::to_string(journal_device_sect)+")"
        );
    }
}
