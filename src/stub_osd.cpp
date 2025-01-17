// Copyright (c) Vitaliy Filippov, 2019+
// License: VNPL-1.1 or GNU GPL-2.0+ (see README.md for details)

/**
 * Stub "OSD" to test & compare network performance with sync read/write and io_uring
 *
 * Core i7-6700HQ laptop
 *
 * stub_osd:
 * randwrite Q1 S1: 36900 iops
 * randwrite Q32 S32: 71000 iops
 * randwrite Q32 S32 (multi-fsync fix): 113000 iops
 * randread Q1: 67300 iops
 * randread Q32: 144000 iops
 *
 * io_uring osd with #define OSD_STUB:
 * randwrite Q1 S1: 30000 iops
 * randwrite Q32 S32: 78600 iops
 * randwrite Q32 S32 (multi-fsync fix): 125000 iops
 * randread Q1: 50700 iops
 * randread Q32: 86100 iops
 *
 * It seems io_uring is fine :)
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#include <stdexcept>

#include "addr_util.h"
#include "rw_blocking.h"
#include "osd_ops.h"

int bind_stub(std::string bind_address, int bind_port);

void run_stub(int peer_fd);

int main(int narg, char *args[])
{
    int listen_fd = bind_stub("0.0.0.0", 11203);
    // Accept new connections
    sockaddr addr;
    socklen_t peer_addr_size = sizeof(addr);
    int peer_fd;
    while (1)
    {
        printf("stub_osd: waiting for 1 client\n");
        peer_fd = accept(listen_fd, &addr, &peer_addr_size);
        if (peer_fd == -1)
        {
            if (errno == EAGAIN)
                continue;
            else
                throw std::runtime_error(std::string("accept: ") + strerror(errno));
        }
        printf("stub_osd: new client %d: connection from %s\n", peer_fd,
            addr_to_string(addr).c_str());
        int one = 1;
        setsockopt(peer_fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
        run_stub(peer_fd);
        close(peer_fd);
        printf("stub_osd: client %d disconnected\n", peer_fd);
        // Try to accept next connection
        peer_addr_size = sizeof(addr);
    }
    return 0;
}

int bind_stub(std::string bind_address, int bind_port)
{
    int listen_backlog = 128;

    sockaddr addr;
    if (!string_to_addr(bind_address, 0, bind_port, &addr))
    {
        throw std::runtime_error("bind address "+bind_address+" is not valid");
    }

    int listen_fd = socket(addr.sa_family, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        throw std::runtime_error(std::string("socket: ") + strerror(errno));
    }
    int enable = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    if (bind(listen_fd, &addr, sizeof(addr)) < 0)
    {
        close(listen_fd);
        throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }

    if (listen(listen_fd, listen_backlog) < 0)
    {
        close(listen_fd);
        throw std::runtime_error(std::string("listen: ") + strerror(errno));
    }

    return listen_fd;
}

void run_stub(int peer_fd)
{
    osd_any_op_t op;
    osd_any_reply_t reply = { 0 };
    void *buf = NULL;
    while (1)
    {
        int r = read_blocking(peer_fd, op.buf, OSD_PACKET_SIZE);
        if (r < OSD_PACKET_SIZE)
        {
            break;
        }
        if (op.hdr.magic != SECONDARY_OSD_OP_MAGIC)
        {
            printf("client %d: bad magic number in operation header\n", peer_fd);
            break;
        }
        reply.hdr.magic = SECONDARY_OSD_REPLY_MAGIC;
        reply.hdr.id = op.hdr.id;
        reply.hdr.opcode = op.hdr.opcode;
        if (op.hdr.opcode == OSD_OP_SEC_READ)
        {
            reply.hdr.retval = op.sec_rw.len;
            buf = malloc(op.sec_rw.len);
            r = write_blocking(peer_fd, reply.buf, OSD_PACKET_SIZE);
            if (r == OSD_PACKET_SIZE)
                r = write_blocking(peer_fd, buf, op.sec_rw.len);
            free(buf);
            if (r < op.sec_rw.len)
                break;
        }
        else if (op.hdr.opcode == OSD_OP_SEC_WRITE || op.hdr.opcode == OSD_OP_SEC_WRITE_STABLE)
        {
            buf = malloc(op.sec_rw.len);
            r = read_blocking(peer_fd, buf, op.sec_rw.len);
            free(buf);
            reply.hdr.retval = op.sec_rw.len;
            if (r == op.sec_rw.len)
                r = write_blocking(peer_fd, reply.buf, OSD_PACKET_SIZE);
            else
                r = 0;
            if (r < OSD_PACKET_SIZE)
                break;
        }
        else if (op.hdr.opcode == OSD_OP_TEST_SYNC_STAB_ALL)
        {
            reply.hdr.retval = 0;
            r = write_blocking(peer_fd, reply.buf, OSD_PACKET_SIZE);
            if (r < OSD_PACKET_SIZE)
                break;
        }
        else
        {
            printf("client %d: unsupported stub opcode: %lu\n", peer_fd, op.hdr.opcode);
            break;
        }
    }
    free(buf);
}
