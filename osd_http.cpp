#include <netinet/tcp.h>
#include <sys/epoll.h>

#include <net/if.h>
#include <ifaddrs.h>

#include <ctype.h>

#include "osd_http.h"
#include "osd.h"

static int extract_port(std::string & host)
{
    int port = 0;
    int pos = 0;
    if ((pos = host.find(':')) >= 0)
    {
        port = strtoull(host.c_str() + pos + 1, NULL, 10);
        if (port >= 0x10000)
        {
            port = 0;
        }
        host = host.substr(0, pos);
    }
    return port;
}

std::vector<std::string> getifaddr_list()
{
    std::vector<std::string> addresses;
    ifaddrs *list, *ifa;
    if (getifaddrs(&list) == -1)
    {
        throw std::runtime_error(std::string("getifaddrs: ") + strerror(errno));
    }
    for (ifa = list; ifa != NULL; ifa = ifa->ifa_next)
    {
        if (!ifa->ifa_addr)
        {
            continue;
        }
        int family = ifa->ifa_addr->sa_family;
        if ((family == AF_INET || family == AF_INET6) &&
            (ifa->ifa_flags & (IFF_UP | IFF_RUNNING | IFF_LOOPBACK)) == (IFF_UP | IFF_RUNNING))
        {
            void *addr_ptr;
            if (family == AF_INET)
                addr_ptr = &((sockaddr_in *)ifa->ifa_addr)->sin_addr;
            else
                addr_ptr = &((sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
            char addr[INET6_ADDRSTRLEN];
            if (!inet_ntop(family, addr_ptr, addr, INET6_ADDRSTRLEN))
            {
                throw std::runtime_error(std::string("inet_ntop: ") + strerror(errno));
            }
            addresses.push_back(std::string(addr));
        }
    }
    freeifaddrs(list);
    return addresses;
}

struct http_co_t
{
    osd_t *osd;
    std::string host;
    std::string request;
    std::string response;
    std::vector<char> rbuf;

    int st = 0;
    int peer_fd = -1;
    int epoll_events = 0;
    int code = 0;
    int sent = 0, received = 0;
    iovec iov;
    msghdr msg = { 0 };
    int cqe_res = 0;

    std::function<void(int, std::string)> callback;
    std::function<void(int, int)> epoll_handler;

    ~http_co_t();
    void resume();
};

void osd_t::http_request(std::string host, std::string request, std::function<void(int, std::string)> callback)
{
    http_co_t *handler = new http_co_t();
    handler->osd = this;
    handler->host = host;
    handler->request = request;
    handler->callback = callback;
    handler->epoll_handler = [this, handler](int peer_fd, int epoll_events)
    {
        handler->epoll_events |= epoll_events;
        handler->resume();
    };
    handler->resume();
}

void osd_t::http_request_json(std::string host, std::string request,
    std::function<void(std::string, json11::Json r)> callback)
{
    http_request(host, request, [this, callback](int err, std::string txt)
    {
        if (err != 0)
        {
            callback("Error code: "+std::to_string(err)+" ("+std::string(strerror(err))+")", json11::Json());
            return;
        }
        std::unique_ptr<http_response_t> res(parse_http_response(txt));
        if (res->status_code != 200)
        {
            callback("HTTP "+std::to_string(res->status_code)+" "+res->status_line+" body: "+res->body, json11::Json());
            return;
        }
        std::string json_err;
        json11::Json data = json11::Json::parse(res->body, json_err);
        if (json_err != "")
        {
            callback("Bad JSON: "+json_err+" (response: "+(res->body == "" ? txt : res->body)+")", json11::Json());
            return;
        }
        callback(std::string(), data);
    });
}

http_response_t *parse_http_response(std::string res)
{
    http_response_t *parsed = new http_response_t();
    int pos = res.find("\r\n");
    pos = pos < 0 ? res.length() : pos+2;
    std::string status_line = res.substr(0, pos);
    int http_version;
    char *status_text = NULL;
    sscanf(status_line.c_str(), "HTTP/1.%d %d %ms", &http_version, &parsed->status_code, &status_text);
    if (status_text)
    {
        parsed->status_line = status_text;
        free(status_text);
        status_text = NULL;
    }
    int prev = pos;
    while ((pos = res.find("\r\n", prev)) >= prev)
    {
        if (pos == prev)
        {
            if (parsed->headers["transfer-encoding"] == "chunked")
            {
                prev = pos+2;
                while ((pos = res.find("\r\n", prev)) >= prev)
                {
                    uint64_t len = strtoull(res.c_str()+prev, NULL, 16);
                    parsed->body += res.substr(pos+2, len);
                    prev = pos+2+len+2;
                }
            }
            else
            {
                parsed->body = res.substr(pos+2);
            }
            break;
        }
        std::string header = res.substr(prev, pos-prev);
        int p2 = header.find(":");
        if (p2 >= 0)
        {
            std::string key = header.substr(0, p2);
            for (int i = 0; i < key.length(); i++)
                key[i] = tolower(key[i]);
            int p3 = p2+1;
            while (p3 < header.length() && isblank(header[p3]))
                p3++;
            parsed->headers[key] = header.substr(p3);
        }
        prev = pos+2;
    }
    return parsed;
}

http_co_t::~http_co_t()
{
    callback(code, response);
    if (peer_fd >= 0)
    {
        osd->epoll_handlers.erase(peer_fd);
        epoll_ctl(osd->epoll_fd, EPOLL_CTL_DEL, peer_fd, NULL);
        close(peer_fd);
        peer_fd = -1;
    }
}

void http_co_t::resume()
{
    if (st == 0)
    {
        int port = extract_port(host);
        struct sockaddr_in addr;
        int r;
        if ((r = inet_pton(AF_INET, host.c_str(), &addr.sin_addr)) != 1)
        {
            code = ENXIO;
            delete this;
            return;
        }
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port ? port : 80);
        peer_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (peer_fd < 0)
        {
            code = errno;
            delete this;
            return;
        }
        fcntl(peer_fd, F_SETFL, fcntl(peer_fd, F_GETFL, 0) | O_NONBLOCK);
        r = connect(peer_fd, (sockaddr*)&addr, sizeof(addr));
        if (r < 0 && errno != EINPROGRESS)
        {
            code = errno;
            delete this;
            return;
        }
        osd->epoll_handlers[peer_fd] = epoll_handler;
        // Add FD to epoll (EPOLLOUT for tracking connect() result)
        epoll_event ev;
        ev.data.fd = peer_fd;
        ev.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP | EPOLLET;
        if (epoll_ctl(osd->epoll_fd, EPOLL_CTL_ADD, peer_fd, &ev) < 0)
        {
            code = errno;
            delete this;
            return;
        }
        epoll_events = 0;
        st = 1;
        return;
    }
    if (st == 1)
    {
        if (epoll_events & (EPOLLOUT | EPOLLERR))
        {
            int result = 0;
            socklen_t result_len = sizeof(result);
            if (getsockopt(peer_fd, SOL_SOCKET, SO_ERROR, &result, &result_len) < 0)
            {
                result = errno;
            }
            if (result != 0)
            {
                code = result;
                delete this;
                return;
            }
            int one = 1;
            setsockopt(peer_fd, SOL_TCP, TCP_NODELAY, &one, sizeof(one));
            // Disable EPOLLOUT on this fd
            epoll_event ev;
            ev.data.fd = peer_fd;
            ev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
            if (epoll_ctl(osd->epoll_fd, EPOLL_CTL_MOD, peer_fd, &ev) < 0)
            {
                code = errno;
                delete this;
                return;
            }
            st = 2;
            epoll_events = 0;
            resume();
            return;
        }
        else if (epoll_events & EPOLLRDHUP)
        {
            delete this;
            return;
        }
        else
        {
            return;
        }
    }
    // Write data
    if (st == 2)
    {
        io_uring_sqe *sqe = osd->ringloop->get_sqe();
        if (!sqe)
            return;
        ring_data_t* data = ((ring_data_t*)sqe->user_data);
        iov = (iovec){ .iov_base = (void*)(request.c_str()+sent), .iov_len = request.size()-sent };
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        data->callback = [this](ring_data_t *data)
        {
            st = 4;
            cqe_res = data->res;
            resume();
        };
        my_uring_prep_sendmsg(sqe, peer_fd, &msg, 0);
        st = 3;
        return;
    }
    if (st == 3)
    {
        return;
    }
    if (st == 4)
    {
        if (cqe_res < 0 && cqe_res != -EAGAIN)
        {
            delete this;
            return;
        }
        sent += cqe_res;
        if (sent < request.size())
            st = 2;
        else
            st = 5;
        resume();
        return;
    }
    // Read response
    if (st == 5)
    {
        if (epoll_events & EPOLLIN)
        {
            if (rbuf.size() != 9000)
                rbuf.resize(9000);
            io_uring_sqe *sqe = osd->ringloop->get_sqe();
            if (!sqe)
                return;
            ring_data_t* data = ((ring_data_t*)sqe->user_data);
            iov = { .iov_base = rbuf.data(), .iov_len = 9000 };
            msg.msg_iov = &iov;
            msg.msg_iovlen = 1;
            data->callback = [this](ring_data_t *data)
            {
                st = 7;
                cqe_res = data->res;
                resume();
            };
            my_uring_prep_recvmsg(sqe, peer_fd, &msg, 0);
            st = 6;
            epoll_events = epoll_events & ~EPOLLIN;
        }
        else if (epoll_events & (EPOLLRDHUP|EPOLLERR))
        {
            delete this;
            return;
        }
    }
    if (st == 6)
    {
        return;
    }
    if (st == 7)
    {
        if (cqe_res < 0 && cqe_res != -EAGAIN)
        {
            delete this;
            return;
        }
        response += std::string(rbuf.data(), cqe_res);
        received += cqe_res;
        st = 5;
        resume();
        return;
    }
}