/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "AbstractCommand.h"
#include "DlAbortEx.h"
#include "DlRetryEx.h"
#include "InitiateConnectionCommandFactory.h"
#include "Util.h"
#include "message.h"
#include "SleepCommand.h"
#include "prefs.h"

AbstractCommand::AbstractCommand(int cuid,
				 const RequestHandle req,
				 DownloadEngine* e,
				 const SocketHandle& s):
  Command(cuid), req(req), e(e), socket(s),
  checkSocketIsReadable(false), checkSocketIsWritable(false),
  nameResolverCheck(false) {
  
  setReadCheckSocket(socket);
  timeout = this->e->option->getAsInt(PREF_TIMEOUT);
}

AbstractCommand::~AbstractCommand() {
  disableReadCheckSocket();
  disableWriteCheckSocket();
}

bool AbstractCommand::execute() {
  try {
    if(e->segmentMan->finished()) {
      logger->debug("CUID#%d - finished.", cuid);
      return true;
    }
    PeerStatHandle peerStat = e->segmentMan->getPeerStat(cuid);
    if(peerStat.get()) {
      if(peerStat->getStatus() == PeerStat::REQUEST_IDLE) {
	logger->info("CUID#%d - Request idle.", cuid);
	onAbort(0);
	req->resetUrl();
	tryReserved();
	return true;
      }
    }
    if(checkSocketIsReadable && readCheckTarget->isReadable(0) ||
       checkSocketIsWritable && writeCheckTarget->isWritable(0) ||
#ifdef ENABLE_ASYNC_DNS
       nameResolverCheck && nameResolveFinished() ||
#endif // ENABLE_ASYNC_DNS
       !checkSocketIsReadable && !checkSocketIsWritable && !nameResolverCheck) {
      checkPoint.reset();
      Segment segment;
      if(e->segmentMan->downloadStarted) {
	if(!e->segmentMan->getSegment(segment, cuid)) {
	  logger->info(MSG_NO_SEGMENT_AVAILABLE, cuid);
	  return prepareForRetry(1);
	}
      }
      return executeInternal(segment);
    } else {

      if(checkPoint.elapsed(timeout)) {
	throw new DlRetryEx(EX_TIME_OUT);
      }
      e->commands.push_back(this);
      return false;
    }
  } catch(DlAbortEx* err) {
    logger->error(MSG_DOWNLOAD_ABORTED, err, cuid);
    onAbort(err);
    delete(err);
    req->resetUrl();
    e->segmentMan->errors++;
    tryReserved();
    return true;
  } catch(DlRetryEx* err) {
    logger->error(MSG_RESTARTING_DOWNLOAD, err, cuid);
    req->addTryCount();
    bool isAbort = e->option->getAsInt(PREF_MAX_TRIES) != 0 &&
      req->getTryCount() >= e->option->getAsInt(PREF_MAX_TRIES);
    if(isAbort) {
      onAbort(err);
    }
    delete(err);
    if(isAbort) {
      logger->error(MSG_MAX_TRY, cuid, req->getTryCount());
      e->segmentMan->errors++;
      tryReserved();
      return true;
    } else {
      return prepareForRetry(e->option->getAsInt(PREF_RETRY_WAIT));
    }
  }
}

void AbstractCommand::tryReserved() {
  if(!e->segmentMan->reserved.empty()) {
    RequestHandle req = e->segmentMan->reserved.front();
    e->segmentMan->reserved.pop_front();
    Command* command = InitiateConnectionCommandFactory::createInitiateConnectionCommand(cuid, req, e);
    e->commands.push_back(command);
  }
}

bool AbstractCommand::prepareForRetry(int wait) {
  e->segmentMan->cancelSegment(cuid);
  Command* command = InitiateConnectionCommandFactory::createInitiateConnectionCommand(cuid, req, e);
  if(wait == 0) {
    e->commands.push_back(command);
  } else {
    SleepCommand* scom = new SleepCommand(cuid, e, command, wait);
    e->commands.push_back(scom);
  }
  return true;
}

void AbstractCommand::onAbort(Exception* ex) {
  logger->debug(MSG_UNREGISTER_CUID, cuid);
  //e->segmentMan->unregisterId(cuid);
  e->segmentMan->cancelSegment(cuid);
}

void AbstractCommand::disableReadCheckSocket() {
  if(checkSocketIsReadable) {
    e->deleteSocketForReadCheck(readCheckTarget, this);
    checkSocketIsReadable = false;
    readCheckTarget = SocketHandle();
  }  
}

void AbstractCommand::setReadCheckSocket(const SocketHandle& socket) {
  if(!socket->isOpen()) {
    disableReadCheckSocket();
  } else {
    if(checkSocketIsReadable) {
      if(readCheckTarget != socket) {
	e->deleteSocketForReadCheck(readCheckTarget, this);
	e->addSocketForReadCheck(socket, this);
	readCheckTarget = socket;
      }
    } else {
      e->addSocketForReadCheck(socket, this);
      checkSocketIsReadable = true;
      readCheckTarget = socket;
    }
  }
}

void AbstractCommand::disableWriteCheckSocket() {
  if(checkSocketIsWritable) {
    e->deleteSocketForWriteCheck(writeCheckTarget, this);
    checkSocketIsWritable = false;
    writeCheckTarget = SocketHandle();
  }
}

void AbstractCommand::setWriteCheckSocket(const SocketHandle& socket) {
  if(!socket->isOpen()) {
    disableWriteCheckSocket();
  } else {
    if(checkSocketIsWritable) {
      if(writeCheckTarget != socket) {
	e->deleteSocketForWriteCheck(writeCheckTarget, this);
	e->addSocketForWriteCheck(socket, this);
	writeCheckTarget = socket;
      }
    } else {
      e->addSocketForWriteCheck(socket, this);
      checkSocketIsWritable = true;
      writeCheckTarget = socket;
    }
  }
}

#ifdef ENABLE_ASYNC_DNS
void AbstractCommand::setNameResolverCheck(const NameResolverHandle& resolver) {
  nameResolverCheck = true;
  e->addNameResolverCheck(resolver, this);
}

void AbstractCommand::disableNameResolverCheck(const NameResolverHandle& resolver) {
  nameResolverCheck = false;
  e->deleteNameResolverCheck(resolver, this);
}

bool AbstractCommand::resolveHostname(const string& hostname,
				      const NameResolverHandle& resolver) {
  switch(resolver->getStatus()) {
  case NameResolver::STATUS_READY:
    logger->info("CUID#%d - Resolving hostname %s", cuid, hostname.c_str());
    resolver->resolve(hostname);
    setNameResolverCheck(resolver);
    return false;
  case NameResolver::STATUS_SUCCESS:
    logger->info("CUID#%d - Name resolution complete: %s -> %s", cuid,
		 hostname.c_str(), resolver->getAddrString().c_str());
    return true;
    break;
  case NameResolver::STATUS_ERROR:
    throw new DlAbortEx("CUID#%d - Name resolution for %s failed:%s", cuid,
			hostname.c_str(),
			resolver->getError().c_str());
  default:
    return false;
  }
}

bool AbstractCommand::nameResolveFinished() const {
  return false;
}
#endif // ENABLE_ASYNC_DNS
