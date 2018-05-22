﻿#include "epoll.h"
#include "epollsocket.h"
#include "epollaccept.h"

#include "../../utils/Log/spdlog/spdlog.h"
#include "../../common.h"

using namespace qyhnetwork;

#ifndef WIN32

bool EventLoop::initialize()
{
    if (_epoll != InvalidFD)
    {
        combined_logger->error("EventLoop::initialize epoll is created ! ") ;
        return false;
    }
    const int IGNORE_ENVENTS = 100;
    _epoll = epoll_create(IGNORE_ENVENTS);
    if (_epoll == InvalidFD)
    {
        combined_logger->error("EventLoop::initialize create epoll err errno=",strerror(errno));
        return false;
    }

    if (socketpair(AF_LOCAL, SOCK_STREAM, 0, _sockpair) != 0)
    {
        combined_logger->error("EventLoop::initialize create socketpair.  errno=" , strerror(errno));
        return false;
    }
    setNonBlock(_sockpair[0]);
    setNonBlock(_sockpair[1]);
    setNoDelay(_sockpair[0]);
    setNoDelay(_sockpair[1]);

    //add socketpair[1] read event to epoll
    _eventData._event.data.ptr = &_eventData;
    _eventData._event.events = EPOLLIN;
    _eventData._fd = _sockpair[1];
    _eventData._linkstat = LS_ESTABLISHED;
    _eventData._type = EventData::REG_ZSUMMER;
    registerEvent(EPOLL_CTL_ADD, _eventData);
    return true;
}

bool EventLoop::registerEvent(int op, EventData & ed)
{
    if (epoll_ctl(_epoll, op, ed._fd, &ed._event) != 0)
    {
        combined_logger->warn("EventLoop::registerEvent error. op={0}, event={1}", op,ed._event.events);
        return false;
    }
    return true;
}

void EventLoop::PostMessage(_OnPostHandler &&handle)
{
    _OnPostHandler * pHandler = new _OnPostHandler(std::move(handle));
    bool needNotice = false;
    _postQueueLock.lock();
    if (_postQueue.empty()){needNotice = true;}
    _postQueue.push_back(pHandler);
    _postQueueLock.unlock();
    if (needNotice)
    {
        char c = '0'; send(_sockpair[0], &c, 1, 0); // safe
    }

}

std::string EventLoop::logSection()
{
    std::stringstream os;
    _postQueueLock.lock();
    MessageStack::size_type msgSize = _postQueue.size();
    _postQueueLock.unlock();
    os << " EventLoop: _epoll=" << _epoll << ", _sockpair[2]={" << _sockpair[0] << "," << _sockpair[1] << "}"
       << " _postQueue.size()=" << msgSize << ", current total timer=" << _timer.getTimersCount()
       << " _eventData=" << _eventData;
    return os.str();
}

void EventLoop::runOnce(bool isImmediately)
{
    int retCount = epoll_wait(_epoll, _events, MAX_EPOLL_WAIT,  isImmediately ? 0 : _timer.getNextExpireTime());
    if (retCount == -1)
    {
        if (errno != EINTR)
        {
            combined_logger->warn("EventLoop::runOnce  epoll_wait err!  errno={0}" ,strerror(errno));
            return; //! error
        }
        return;
    }

    //check timer
    {
        _timer.checkTimer();
        if (retCount == 0) return;//timeout
    }


    for (int i=0; i<retCount; i++)
    {
        EventData * pEvent = (EventData *)_events[i].data.ptr;
        //tagHandle  type
        if (pEvent->_type == EventData::REG_ZSUMMER)
        {
            {
                char buf[200];
                while (recv(pEvent->_fd, buf, 200, 0) > 0);
            }

            MessageStack msgs;
            _postQueueLock.lock();
            msgs.swap(_postQueue);
            _postQueueLock.unlock();

            for (auto pfunc : msgs)
            {
                _OnPostHandler * p = (_OnPostHandler*)pfunc;
                try
                {
                    (*p)();
                }
                catch (const std::exception & e)
                {
                    combined_logger->warn("OnPostHandler have runtime_error exception. err={0}",e.what());
                }
                catch (...)
                {
                    combined_logger->warn("OnPostHandler have unknown exception.");
                }
                delete p;
            }
        }
        else if (pEvent->_type == EventData::REG_TCP_ACCEPT)
        {
            if (pEvent->_tcpacceptPtr)
            {
                pEvent->_tcpacceptPtr->onEPOLLMessage(_events[i].events);
            }
        }
        else if (pEvent->_type == EventData::REG_TCP_SOCKET)
        {
            if (pEvent->_tcpSocketPtr)
            {
                pEvent->_tcpSocketPtr->onEPOLLMessage(_events[i].events);
            }
        }
        else
        {
            combined_logger->error("EventLoop::runOnce check register event type failed !!  type={0}",pEvent->_type );
        }

    }
}

#endif
