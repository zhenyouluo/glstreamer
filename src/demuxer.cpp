#include <iostream>
#include <tuple>

#include <unistd.h>

#include "demuxer.h"

#include "exceptions.h"
#include "fdpump.h"
#include "prettyio.h"
#include "protocoltypes.h"
#include "posixexception.h"

using namespace glstreamer;

Demuxer::Demuxer( const sockaddr* bindAddr, socklen_t addrLen ):
listenfd(socket(bindAddr->sa_family, SOCK_STREAM, 0)),
pumps(),
stateLock()
{
    if(listenfd.fd() < 0)
        throw_posix(socket);
    
    int reuse_addr = 1;
    int err = ::setsockopt(listenfd.fd(), SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
    if(err)
        throw_posix(setsockopt);
    
    err = ::bind(listenfd.fd(), bindAddr, addrLen);
    if(err)
        throw_posix(bind);
    
    err = listen(listenfd.fd(), 16);
    if(err)
        throw_posix(listen);
    
    this->start();
}

Demuxer::~Demuxer()
{
    this->cancel();
    this->waitForFinish();
}

LinkBuffer* Demuxer::getLink ( Word32 linkId, size_type bufferCount, TypeSpec* typeSpec )
{
    std::lock_guard<std::mutex> stateLocker(stateLock);
    
    auto iter = pumps.find(linkId);
    if(iter == pumps.end())
    {
        std::unique_ptr<FdPump> pump(new FdPump);
        std::tie(iter, std::ignore) = pumps.insert(std::make_pair(linkId, std::move(pump)));
    }
    return iter->second->allocateBuffer(bufferCount, typeSpec);
}

void Demuxer::run()
{
    try
    {
        CancellationEnabler cancellation_on;
        while(true)
        {
            PosixFd fd(cancel_point(accept(listenfd.fd(), nullptr, nullptr), ::glstreamer::negerrno));
            if(fd.fd() < 0)
                throw_posix(accept);
            
            LinkHeader linkHeader;
            readFd(fd.fd(), linkHeader);
            
            if(linkHeader.magic != linkHeader.magicValue)
            {
                writeFd(fd.fd(), NetUInt32(ErrorCode::errMagic));
                continue;
            }
            
            if(linkHeader.crc != crc32(linkHeader))
            {
                writeFd(fd.fd(), NetUInt32(ErrorCode::errCrc));
                continue;
            }
            
            std::lock_guard<std::mutex> stateLocker(stateLock);
            auto iter = pumps.find(linkHeader.linkId);
            if(iter == pumps.end())
            {
                std::unique_ptr<FdPump> pump(new FdPump);
                std::tie(iter, std::ignore) = pumps.insert(std::make_pair(linkHeader.linkId, std::move(pump)));
            }
            try {
                iter->second->assignLink(fd.fd(), linkHeader);
            } catch(ResourceConflict const&)
            {
                writeFd(fd.fd(), NetUInt32(ErrorCode::errExist));
            }
            fd.release();
        }
    }
    catch(::glstreamer::CancelException const&)
    {}
    catch(std::exception const& e)
    {
        std::cerr << e.what() << std::endl;
    }
}
