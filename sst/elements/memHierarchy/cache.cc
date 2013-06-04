// Copyright 2009-2013 Sandia Corporation. Under the terms
// of Contract DE-AC04-94AL85000 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2013, Sandia Corporation
// All rights reserved.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#include <sstream>
#include <string>
#include <algorithm>
#include <iomanip>
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <inttypes.h>

#define ASSERT(x) \
    do { \
        if (!(x) ) { \
            fflush(stdout); \
            fprintf(stderr, "%s:%d  Assert failed: '%s'\n", __FILE__, __LINE__, #x); \
            abort(); \
        } \
    } while(0)

#include <sst_config.h>
#include <sst/core/serialization/element.h>
#include <sst/core/simulation.h>
#include <sst/core/element.h>
#include <sst/core/interfaces/memEvent.h>
#include <sst/core/interfaces/stringEvent.h>

#include "cache.h"



#define DPRINTF( fmt, args...) __DBG( DBG_CACHE, Cache, "%s: " fmt, getName().c_str(), ## args )

using namespace SST;
using namespace SST::MemHierarchy;
using namespace SST::Interfaces;

static const std::string NO_NEXT_LEVEL = "NONE";

Cache::Cache(ComponentId_t id, Params_t& params) :
	Component(id)
{

	// get parameters
	n_ways = params.find_integer("num_ways", 0);
	n_rows = params.find_integer("num_rows", 0);
	blocksize = params.find_integer("blocksize", 0);
	if ( n_ways == 0 || n_rows == 0 || blocksize == 0 ) {
		_abort(Cache, "# Ways, # Rows and Blocksize must all be >0\n");
	}

    std::string cacheType = params.find_string("mode", "STANDARD");
    std::transform(cacheType.begin(), cacheType.end(), cacheType.begin(), ::toupper);
    if ( cacheType == "INCLUSIVE" ) {
        cacheMode = INCLUSIVE;
    } else if ( cacheType == "EXCLUSIVE" ) {
        _abort(Cache, "Cache mode EXCLUSIVE not yet implemented.\n");
        cacheMode = EXCLUSIVE;
    } else if ( cacheType == "STANDARD" ) {
        cacheMode = STANDARD;
    } else
        _abort(Cache, "Cache 'mode' must be one of 'INCLUSIVE', 'EXCLUSIVE' or 'STANDARD' (default)\n");


	//  TODO:  Is this right?
	registerTimeBase("2 ns", true);

	n_upstream = params.find_integer("num_upstream", 0);
	if ( n_upstream > 0 ) {
		upstream_links = new SST::Link*[n_upstream];
		for ( int i = 0 ; i < n_upstream ; i++ ) {
			std::ostringstream linkName;
			linkName << "upstream" << i;
			std::string ln = linkName.str();
			upstream_links[i] = configureLink( ln, "50 ps",
					new Event::Handler<Cache, SourceType_t>(this,
						&Cache::handleIncomingEvent, UPSTREAM) );
			ASSERT(upstream_links[i]);
			upstreamLinkMap[upstream_links[i]->getId()] = i;
            DPRINTF("upstream_links[%d]->getId() = %ld\n", i, upstream_links[i]->getId());
		}
	}

	next_level_name = params.find_string("next_level", NO_NEXT_LEVEL);
	downstream_link = configureLink( "downstream",
			new Event::Handler<Cache, SourceType_t>(this,
				&Cache::handleIncomingEvent, DOWNSTREAM) );
    if ( downstream_link )
        DPRINTF("Downstream Link id = %ld\n", downstream_link->getId());
	snoop_link = configureLink( "snoop_link", "50 ps",
			new Event::Handler<Cache, SourceType_t>(this,
				&Cache::handleIncomingEvent, SNOOP) );
	if ( snoop_link != NULL ) { // Snoop is a bus.
		snoopBusQueue.setup(this, snoop_link);
        DPRINTF("SNOOP Link id = %ld\n", snoop_link->getId());
	}

    /* TODO:  Check to see if 'directory_link' exists */
    if ( isPortConnected("directory_link") ) {
        MemNIC::ComponentInfo myInfo;
        myInfo.link_port = "directory_link";
        myInfo.link_bandwidth = "2 ns"; // Time base as registered earlier
        myInfo.name = getName();
        myInfo.network_addr = params.find_integer("net_addr");
        myInfo.type = MemNIC::TypeCache;
        myInfo.typeInfo.cache.blocksize = blocksize;
        myInfo.typeInfo.cache.num_blocks = n_ways * n_rows;

        directory_link = new MemNIC(this, myInfo,
                new Event::Handler<Cache, SourceType_t>(this,
                    &Cache::handleIncomingEvent, DIRECTORY) );
    } else {
        directory_link = NULL;
    }

	self_link = configureSelfLink("Self", params.find_string("access_time", ""),
				new Event::Handler<Cache>(this, &Cache::handleSelfEvent));

	rowshift = numBits(blocksize);
	rowmask = n_rows - 1;  // Assumption => n_rows is power of 2
	tagshift = numBits(blocksize) + numBits(n_rows);



	database = std::vector<CacheRow>(n_rows, CacheRow(this));
	for ( int r = 0 ; r < n_rows ; r++ ) {
		for ( int c = 0 ; c < n_ways ; c++ ) {
			database[r].blocks[c].row = r;
			database[r].blocks[c].col = c;
		}
	}


	num_read_hit = 0;
	num_read_miss = 0;
	num_supply_hit = 0;
	num_supply_miss = 0;
	num_write_hit = 0;
	num_write_miss = 0;
	num_upgrade_miss = 0;

    registerClock("1 GHz", new Clock::Handler<Cache>(this, &Cache::clockTick));

    std::string prefetcher = params.find_string("prefetcher");
    if ( prefetcher == "" ) {
        listener = new CacheListener();
    } else {
        listener = dynamic_cast<CacheListener*>(loadModule(prefetcher, params));

        if(NULL == listener) {
            _abort(Cache, "Prefetcher could not be loaded.\n");
        }
    }
    listener->registerResponseCallback(new Event::Handler<Cache>(this, &Cache::handlePrefetchEvent));

    /* L1 status will be detected by seeing CPU requests come in. */
    isL1 = false;
}


bool Cache::clockTick(Cycle_t) {
    if ( directory_link ) {
        directory_link->clock();
    }
    return false;
}


void Cache::init(unsigned int phase)
{
    if ( directory_link ) directory_link->init(phase);
	if ( !phase ) {
		for ( int i = 0 ; i < n_upstream ; i++ ) {
			upstream_links[i]->sendInitData(new StringEvent("SST::Interfaces::MemEvent"));
		}
		if ( downstream_link ) downstream_link->sendInitData(new StringEvent("SST::Interfaces::MemEvent"));
		if ( snoop_link ) snoop_link->sendInitData(new StringEvent("SST::Interfaces::MemEvent"));
	}


	/* Cache should only initialize from upstream.  */
	for ( int i = 0 ; i < n_upstream ; i++ ) {
		SST::Event *ev = NULL;
		while ( (ev = upstream_links[i]->recvInitData()) != NULL ) {
			MemEvent *me = dynamic_cast<MemEvent*>(ev);
			if ( me ) {
				/* TODO:  Break down to cache-line size */
				// Pass down
				if ( downstream_link ) downstream_link->sendInitData(me);
				else if ( snoop_link ) snoop_link->sendInitData(me);
				else if ( directory_link ) directory_link->sendInitData(me);
				else delete me;
			} else {
				delete ev;
			}
		}
	}

	/* pass downstream (if possible ) anything coming over snoopy */
    if ( snoop_link ) {
        SST::Event *ev = NULL;
        while ( (ev = snoop_link->recvInitData()) != NULL ) {
			MemEvent *me = dynamic_cast<MemEvent*>(ev);
			if ( me ) {
				if ( directory_link ) directory_link->sendInitData(me);
                else if ( downstream_link ) downstream_link->sendInitData(me);
                else delete me;
            } else {
                delete ev;
            }
        }
    }

}


void Cache::setup(void)
{
    if ( directory_link ) {
        directory_link->setup();
        const std::vector<MemNIC::ComponentInfo> &peers = directory_link->getPeerInfo();
        for ( std::vector<MemNIC::ComponentInfo>::const_iterator i = peers.begin() ; i != peers.end() ; ++i ) {
            if ( i->type == MemNIC::TypeDirectoryCtrl ) {
                /* Record directory controller info */
                directories.push_back(*i);
            }
        }
        // Save some memory
        directory_link->clearPeerInfo();
    }
}


void Cache::finish(void)
{
	printf("Cache %s stats:\n"
			"\t# Read    Hits:      %"PRIu64"\n"
			"\t# Read    Misses:    %"PRIu64"\n"
			"\t# Supply  Hits:      %"PRIu64"\n"
			"\t# Supply  Misses:    %"PRIu64"\n"
			"\t# Write   Hits:      %"PRIu64"\n"
			"\t# Write   Misses:    %"PRIu64"\n"
			"\t# Upgrade Misses:    %"PRIu64"\n",
			getName().c_str(),
			num_read_hit, num_read_miss,
			num_supply_hit, num_supply_miss,
			num_write_hit, num_write_miss,
			num_upgrade_miss);
    if ( DBG_CACHE & SST::_debug_flags )
        printCache();
}


void Cache::handleIncomingEvent(SST::Event *event, SourceType_t src)
{
	handleIncomingEvent(event, src, true);
}

void Cache::handleIncomingEvent(SST::Event *event, SourceType_t src, bool firstTimeProcessed, bool firstPhaseComplete)
{
	MemEvent *ev = static_cast<MemEvent*>(event);
    DPRINTF("Received Event %p (%"PRIu64", %d) (%s to %s (link %ld)) %s 0x%"PRIx64"\n", ev,
            ev->getID().first, ev->getID().second,
            ev->getSrc().c_str(), ev->getDst().c_str(), ev->getLinkId(),
            CommandString[ev->getCmd()], ev->getAddr());
    switch (ev->getCmd()) {
    case BusClearToSend:
        snoopBusQueue.clearToSend(ev);
        delete ev;
        break;

    case ReadReq:
    case WriteReq:
        handleCPURequest(ev, firstTimeProcessed);
        break;

    case RequestData:
        handleCacheRequestEvent(ev, src, firstTimeProcessed);
        break;

    case SupplyData:
        handleCacheSupplyEvent(ev, src);
        break;

    case Invalidate:
        handleInvalidate(ev, src, firstPhaseComplete);
        break;

    case ACK:
        ackInvalidate(ev);
        break;

    case NACK:
        handleNACK(ev, src);
        break;

    case Fetch:
        handleFetch(ev, false, firstPhaseComplete);
        break;
    case FetchInvalidate:
        handleFetch(ev, true, firstPhaseComplete);
        break;

    default:
        /* Ignore */
        delete event;
        break;
    }
}



void Cache::handleSelfEvent(SST::Event *event)
{
	SelfEvent *ev = static_cast<SelfEvent*>(event);
    ev->fire();
    delete ev;
}

void Cache::retryEvent(MemEvent *ev, CacheBlock *block, SourceType_t src)
{
	handleIncomingEvent(ev, src, false);
}


void Cache::handlePrefetchEvent(SST::Event *event)
{
    DPRINTF("Incoming PREFETCHER Event!\n");
    handleIncomingEvent(event, PREFETCHER, true, false);
}



void Cache::handleCPURequest(MemEvent *ev, bool firstProcess)
{
    isL1 = true;
	ASSERT(ev->getCmd() == ReadReq || ev->getCmd() == WriteReq);
	bool isRead = (ev->getCmd() == ReadReq);
	CacheBlock *block = findBlock(ev->getAddr(), false);
	DPRINTF("(%"PRIu64", %d) 0x%"PRIx64"%s %s %s (block 0x%"PRIx64" [%d])\n",
			ev->getID().first, ev->getID().second,
			ev->getAddr(),
            ev->queryFlag(MemEvent::F_LOCKED) ? " [LOCKED]" : "",
			isRead ? "READ" : "WRITE",
			(block) ? ((isRead || block->status == CacheBlock::EXCLUSIVE) ? "HIT" : "UPGRADE") : "MISS",
			addrToBlockAddr(ev->getAddr()),
            block ? block->status : -1
            );

    if ( firstProcess)
        listener->notifyAccess(isRead ? CacheListener::READ : CacheListener::WRITE,
                (block != NULL) ? CacheListener::HIT : CacheListener::MISS,
                ev->getAddr());

    if ( ev->queryFlag(MemEvent::F_LOCKED) && !isRead ) assert(block->status == CacheBlock::EXCLUSIVE);
    if ( block ) {
        /* HIT */
        if ( isRead ) {
            if ( firstProcess ) num_read_hit++;
            if ( waitingForInvalidate(block->baseAddr) ) {
                DPRINTF("Invalidation for this in progress.  Putting into queue.\n");
                invalidations[block->baseAddr].waitingEvents.push_back(std::make_pair(ev, UPSTREAM));
            } else {
                if ( ev->queryFlag(MemEvent::F_LOCKED) && (block->status != CacheBlock::EXCLUSIVE ) ) {
                    issueInvalidate(ev, UPSTREAM, block, CacheBlock::EXCLUSIVE, SEND_BOTH);
                } else {
                    if ( ev->queryFlag(MemEvent::F_LOCKED) ) {
                        supplyMap_t::key_type supplyMapKey = std::make_pair(block->baseAddr, SNOOP);
                        supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
                        if ( block->wb_in_progress || (supMapI != supplyInProgress.end() && supMapI->second.canceled == false)) {
                            /* We still have this in exclusive, but a writeback is in progress.
                             * this will take us out of exclusive.  Let's punt and retry later.
                             */
                            DPRINTF("There's a WB (%d) or a Supply in progress.  Retry this locked event later.\n", block->wb_in_progress);
                            self_link->send(1, new SelfEvent(this, &Cache::retryEvent, ev, block, UPSTREAM));
                            return;
                        }
                        /* TODO!   We lock on (bytes/words), but the line is locked! */
                        block->user_locked++;
                        block->user_lock_needs_wb = false;
                    }
                    self_link->send(1, new SelfEvent(this, &Cache::sendCPUResponse, makeCPUResponse(ev, block, UPSTREAM), block, UPSTREAM));
                    delete ev;
                }
            }
        } else {
            if ( block->status == CacheBlock::EXCLUSIVE ) {
                if ( firstProcess ) num_write_hit++;
                updateBlock(ev, block);
                self_link->send(1, new SelfEvent(this, &Cache::sendCPUResponse, makeCPUResponse(ev, block, UPSTREAM), block, UPSTREAM));
                if ( block->user_locked && ev->queryFlag(MemEvent::F_LOCKED) ) {
                    /* Unlock */
                    ASSERT(block->user_locked);
                    block->user_locked--;
                    if ( block->user_locked == 0 && block->user_lock_needs_wb ) {
                        writebackBlock(block, CacheBlock::SHARED);
                    }
                }
                delete ev;
            } else {
                if ( firstProcess ) num_upgrade_miss++;
                if ( waitingForInvalidate(block->baseAddr) ) {
                    DPRINTF("Invalidation for this in progress.  Putting into queue.\n");
                    invalidations[block->baseAddr].waitingEvents.push_back(std::make_pair(ev, UPSTREAM));
                } else {
                    issueInvalidate(ev, UPSTREAM, block, CacheBlock::EXCLUSIVE, SEND_BOTH);
                }
            }
        }
        block->last_touched = getCurrentSimTime();
    } else {
		if ( firstProcess ) {
			if ( isRead )
				num_read_miss++;
			else
				num_write_miss++;
		}
		/* Miss */
		loadBlock(ev, UPSTREAM);
	}
}

static const char* printData(MemEvent *ev) {
    static char buffer[1024] = {0};
    memset(buffer, '\0', sizeof(buffer));
    char *p = buffer;

    p += sprintf(p, "0x");

    size_t nb = ev->getSize();
    std::vector<uint8_t> &data = ev->getPayload();
    for ( size_t i = 0 ; i < nb ; i++ ) {
        p += sprintf(p, "%02x", data[i]);
    }

    return buffer;
}

MemEvent* Cache::makeCPUResponse(MemEvent *ev, CacheBlock *block, SourceType_t src)
{
	Addr offset = ev->getAddr() - block->baseAddr;
	if ( offset+ev->getSize() > (Addr)blocksize ) {
		_abort(Cache, "Cache doesn't handle split rquests.\nReq for addr 0x%"PRIx64" has offset of %"PRIu64", and size %u.  Blocksize is %u\n",
				ev->getAddr(), offset, ev->getSize(), blocksize);
	}

	MemEvent *resp = ev->makeResponse(this);
	if ( ev->getCmd() == ReadReq)
		resp->setPayload(ev->getSize(), &block->data[offset]);

	DPRINTF("Creating Response to CPU: (%"PRIu64", %d) in Response To (%"PRIu64", %d) [%s: 0x%"PRIx64"] [%s]\n",
			resp->getID().first, resp->getID().second,
			resp->getResponseToID().first, resp->getResponseToID().second,
			CommandString[resp->getCmd()], resp->getAddr(),
            printData((ev->getCmd() == ReadReq) ? resp : ev)
           );

	return resp;
}

void Cache::sendCPUResponse(MemEvent *ev, CacheBlock *block, SourceType_t src)
{
    DPRINTF("Sending CPU Response %s 0x%"PRIx64"  (%"PRIu64", %d)\n",
            CommandString[ev->getCmd()], ev->getAddr(),
            ev->getID().first, ev->getID().second);

	/* CPU is always upstream link 0 */
	upstream_links[0]->send(ev);

    /* release events pending on this row */
    CacheRow *row = findRow(ev->getAddr());
    handlePendingEvents(row, NULL);
}


void Cache::issueInvalidate(MemEvent *ev, SourceType_t src, CacheBlock *block, CacheBlock::BlockStatus newStatus, ForwardDir_t direction, bool cancelable)
{
    block->lock();
    invalidations[block->baseAddr].block = block;
    invalidations[block->baseAddr].newStatus = newStatus;
    issueInvalidate(ev, src, block->baseAddr, direction, cancelable);
}


void Cache::issueInvalidate(MemEvent *ev, SourceType_t src, Addr addr, ForwardDir_t direction, bool cancelable)
{
    DPRINTF("Enqueuing request to Invalidate block 0x%"PRIx64"\n", addr);

    Invalidation &inv = invalidations[addr];
    inv.waitingEvents.push_back(std::make_pair(ev, src));
    inv.waitingACKs = 0;
    inv.canCancel = cancelable;

    MemEvent *invalidateEvent = new MemEvent(this, addr, Invalidate);
    inv.issuingEvent = invalidateEvent->getID();

	if ( snoop_link ) {
		MemEvent *invEvent = new MemEvent(invalidateEvent);
		snoopBusQueue.request( invEvent, NULL);
        inv.waitingACKs++;
        inv.busEvent = invEvent;
	}

    if ( direction == SEND_DOWN || direction == SEND_BOTH ) {
        if ( downstream_link && next_level_name != NO_NEXT_LEVEL ) {
            downstream_link->send(new MemEvent(invalidateEvent));
            inv.waitingACKs++;
        }
        if ( directory_link ) {
            //printf("%s: issuing Invalidate (%lu, %d) to Directory for 0x%"PRIx64"\n", getName().c_str(), invalidateEvent->getID().first, invalidateEvent->getID().second, invalidateEvent->getAddr()); fflush(stdout);
            directory_link->send(new MemEvent(invalidateEvent));
            inv.waitingACKs++;
        }
    }

    if ( direction == SEND_UP || direction == SEND_BOTH ) {
        for ( int i = 0 ; i < n_upstream ; i++ ) {
            /* Don't issue invalidate to the person that sent us the command that's causing
             * us to invalidate.
             *
             * Exemption:  Invalidating a block (due to INCLUSIVEness replacement)
             *             This can be checked by if the block's address doesn't
             *             match the event's address
             */
            if ( upstream_links[i]->getId() != ev->getLinkId() || (addrToBlockAddr(ev->getAddr()) != addr) ) {
                upstream_links[i]->send(new MemEvent(invalidateEvent));
                inv.waitingACKs++;
            } else {
                DPRINTF("_______________________________ Working event (%"PRIu64", %d) Odd... (%ld != %ld || 0x%"PRIx64" != 0x%"PRIx64") returned false.\n", ev->getID().first, ev->getID().second, upstream_links[i]->getId(), ev->getLinkId(), addrToBlockAddr(ev->getAddr()), addr);
            }
        }
    }
    DPRINTF("Expecting %d acknowledgments\n", inv.waitingACKs);
    if ( inv.waitingACKs == 0 ) { //a.k.a., nobody to worry about waiting for
        finishIssueInvalidate(addr);
    }
    delete invalidateEvent;

}


void Cache::finishIssueInvalidate(Addr addr)
{
    ASSERT(invalidations[addr].waitingACKs == 0);

    CacheBlock *block = invalidations[addr].block;
    if ( block ) {
        block->unlock();
        block->status = invalidations[addr].newStatus;
    }

    DPRINTF("Received all invalidate ACKs for block 0x%"PRIx64"\n", addr);


    std::deque<std::pair<MemEvent*, SourceType_t> > waitingEvents = invalidations[addr].waitingEvents;
    /* Erase before processing, otherwise, we'll think we're still waiting for invalidations */
    invalidations.erase(addr);
    //printf("%s: Erased Invalidation for 0x%"PRIx64"\n", getName().c_str(), addr); fflush(stdout);
    bool first = true;
    while ( waitingEvents.size() > 0 ) {
        std::pair<MemEvent*, SourceType_t> ev2 = waitingEvents.front();
        waitingEvents.pop_front();
        DPRINTF("Handling formerly blocked event (%"PRIu64", %d) [%s: 0x%"PRIx64"]\n",
                ev2.first->getID().first, ev2.first->getID().second,
                CommandString[ev2.first->getCmd()], ev2.first->getAddr());
        handleIncomingEvent(ev2.first, ev2.second, false, first);
        first = false;
    }

}



/* Takes ownership of the event 'ev'. */
void Cache::loadBlock(MemEvent *ev, SourceType_t src)
{

    std::pair<LoadInfo_t*, bool> initRes = initLoad(ev, src);
    LoadInfo_t *li = initRes.first;
    bool reprocess = !initRes.second;

    /* Check to see if this a reprocess of the head event */
    if ( reprocess && li->initiatingEvent != ev->getID() ) {
        DPRINTF("Adding to existing outstanding Load for this block.\n");
        li->list.push_back(LoadInfo_t::LoadElement_t(ev, src, getCurrentSimTime()));
        return;
    }
    CacheRow *row = findRow(ev->getAddr());
    CacheBlock *block = row->getLRU();

    if ( !block ) { // Row is full, wait for one to be available
        row->addWaitingEvent(ev, src);
        return;
    } else {
        if ( cacheMode == INCLUSIVE ) {
            if ( block->status != CacheBlock::INVALID ) {
                DPRINTF("Replacing a block to handle load.  Need to invalidate any upstream copies of old cache block 0x%"PRIx64".\n", block->baseAddr);
                /* We can go straight to INVALID...
                 *  INCLUSIVE cache's aren't L1 (why bother?)
                 *  Upstream caches either don't have it, have it in SHARED (clean)
                 *  Or EXCLUSIVE, in which case they'll write back before ack'ing.
                 */
                issueInvalidate(ev, src, block, CacheBlock::INVALID, SEND_UP);
                return;
            }
        }
        if ( block->status == CacheBlock::EXCLUSIVE ) {
            DPRINTF("Need to evict block 0x%"PRIx64" to satisfy load for 0x%"PRIx64"\n",
                    block->baseAddr, ev->getAddr());

            row->addWaitingEvent(ev, src);
            writebackBlock(block, CacheBlock::INVALID); // We'll get it next time

            return;
        } else {
            DPRINTF("Replacing block (old status is [%d], 0x%"PRIx64" [%s]\n",
                    block->status, block->baseAddr,
                    block->locked ? "LOCKED" : "unlocked");

        }
    }

    /* Simple Load */
    block->activate(ev->getAddr());
    block->lock();

    li->loadDirection = SEND_DOWN;
    li->targetBlock = block;
    block->loadInfo = li;
    if ( reprocess )
        li->list.push_front(LoadInfo_t::LoadElement_t(ev, src, getCurrentSimTime()));
    else
        li->list.push_back(LoadInfo_t::LoadElement_t(ev, src, getCurrentSimTime()));

    self_link->send(1, new SelfEvent(this, &Cache::finishLoadBlock, li, block->baseAddr, block));
}


std::pair<Cache::LoadInfo_t*,bool> Cache::initLoad(MemEvent *ev, SourceType_t src)
{
    bool initialEvent = false;

    Addr blockAddr = addrToBlockAddr(ev->getAddr());
	LoadList_t::iterator i = waitingLoads.find(blockAddr);
    LoadInfo_t *li = NULL;
	if ( i != waitingLoads.end() ) {
		li = i->second;
		ASSERT (li->addr == blockAddr);
        initialEvent = false;

    } else {
        li = new LoadInfo_t(blockAddr);
        DPRINTF("No existing Load for this block.  Creating.  [li: %p]\n", li);
        std::pair<LoadList_t::iterator, bool> res = waitingLoads.insert(std::make_pair(blockAddr, li));
        li->initiatingEvent = ev->getID();
        initialEvent = true;
    }

    return std::make_pair(li, initialEvent);

}


void Cache::finishLoadBlock(LoadInfo_t *li, Addr addr, CacheBlock *block)
{
    DPRINTF("Time to send load for 0x%"PRIx64"\n", addr);

    /* Check to see if we're still in ASSIGNED state.  If not, we've probably
     * already been processed.
     * Unless this is a SEND_UP fetch, and we're DIRTY.
     */
    if ( !(block->status == CacheBlock::DIRTY && li->loadDirection == SEND_UP) &&
            block->status != CacheBlock::ASSIGNED || block->baseAddr != addr || li != block->loadInfo) {
        DPRINTF("Not going to bother loading.  Somebody else has moved block 0x%"PRIx64" to state [%d]\n",
                block->baseAddr, block->status);
        return;
    }

    if ( li->loadDirection == SEND_UP ) {
        if ( n_upstream > 0 && !isL1 ) {
            for ( int i = 0 ; i < n_upstream ; i++ ) {
                MemEvent *req = new MemEvent(this, block->baseAddr, RequestData);
                req->setSize(blocksize);
                upstream_links[i]->send(req);
            }
        } else if ( snoop_link ) {
            MemEvent *req = new MemEvent(this, block->baseAddr, RequestData);
            req->setSize(blocksize);
            if ( next_level_name != NO_NEXT_LEVEL ) req->setDst(next_level_name);
            DPRINTF("Enqueuing request to load block 0x%"PRIx64"  [li = %p]\n", block->baseAddr, li);
            BusHandlerArgs args;
            args.loadBlock.loadInfo = li;
            li->busEvent = req;
            snoopBusQueue.request( req, new BusFinishHandler(&Cache::finishLoadBlockBus, args));
        }
    } else {

    /* Ordering here...  If you have both downstream & snoop, you're probably
     * at the end of the line.  You don't need to issue on snoop, somebody else
     * probably did.  Just send the load down the line to memory.
     */
    if ( downstream_link ) {
        DPRINTF("Sending request to load block 0x%"PRIx64"  [li = %p]\n", block->baseAddr, li);
        MemEvent *req = new MemEvent(this, block->baseAddr, RequestData);
        req->setSize(blocksize);
        downstream_link->send(req);
    } else if ( directory_link ) {
        DPRINTF("Sending request to Directory to load block 0x%"PRIx64"  [li = %p]\n", block->baseAddr, li);
        MemEvent *req = new MemEvent(this, block->baseAddr, RequestData);
        req->setSize(blocksize);
        req->setDst(findTargetDirectory(block->baseAddr));
        directory_link->send(req);
    } else if ( snoop_link ) {
        MemEvent *req = new MemEvent(this, block->baseAddr, RequestData);
        req->setSize(blocksize);
        if ( next_level_name != NO_NEXT_LEVEL ) req->setDst(next_level_name);
        DPRINTF("Enqueuing request to load block 0x%"PRIx64"  [li = %p]\n", block->baseAddr, li);
        BusHandlerArgs args;
        args.loadBlock.loadInfo = li;
        li->busEvent = req;
        snoopBusQueue.request( req, new BusFinishHandler(&Cache::finishLoadBlockBus, args));
    }
    }
}


void Cache::finishLoadBlockBus(BusHandlerArgs &args)
{
	args.loadBlock.loadInfo->busEvent = NULL;
}



void Cache::handleCacheRequestEvent(MemEvent *ev, SourceType_t src, bool firstProcess)
{
	if ( src == SNOOP && ev->getSrc() == getName() ) {
        delete ev;
        return; // We sent it, ignore
    }

	CacheBlock *block = findBlock(ev->getAddr(), false);
	DPRINTF("0x%"PRIx64" %s %s (block 0x%"PRIx64" [%d.%d])%s\n", ev->getAddr(),
			(src == SNOOP && ev->getDst() != getName()) ? "SNOOP" : "",
			(block) ? "HIT" : "MISS",
			addrToBlockAddr(ev->getAddr()),
            (block) ? block->status : -1,
            (block) ? block->user_locked : -1,
            (block && block->isLocked()) ? " Block LOCKED" : "");

    if ( ev->getSize() != blocksize ) {
        _abort(Cache, "It appears that not all cache line/block sizes are equal.  Unsupported!\n");
    }

	if ( block ) {
        if ( block->status == CacheBlock::DIRTY ) {
            if ( src == SNOOP ) {
                /* Pretend we don't have it.  Somebody else will supply it. */
                delete ev;
                return;
            } else {
                /* TODO  - maybe? */
                assert(false);
            }
        }

		if ( firstProcess ) {
            listener->notifyAccess(CacheListener::READ, CacheListener::HIT, ev->getAddr());
            num_supply_hit++;
        }
		supplyMap_t::key_type supplyMapKey = std::make_pair(block->baseAddr, src);
		/* Hit */

		supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
		if ( supMapI != supplyInProgress.end() && !supMapI->second.canceled ) {
			// we're already working on this
			DPRINTF("Detected that we're already working on this\n");
            delete ev;
			return;
		}

        if ( waitingForInvalidate(block->baseAddr) ) {
            DPRINTF("Invalidation for this in progress.  Putting into queue.\n");
            invalidations[block->baseAddr].waitingEvents.push_back(std::make_pair(ev, src));
            return;
        }

        DPRINTF("CacheRequest Hit for 0x%"PRIx64", will supply data\n", block->baseAddr);
        if ( block->wb_in_progress ) {
            DPRINTF("There's a WB in progress.  That will suffice.\n");
            delete ev;
        } else {
            supplyInProgress[supplyMapKey] = SupplyInfo(NULL);
            block->lock();
            block->last_touched = getCurrentSimTime();
            self_link->send(1, new SelfEvent(this, &Cache::supplyData, ev, block, src));
        }
	} else {
		/* Miss */
        if ( src == DOWNSTREAM ) {
            DPRINTF("DOWNSTREAM request for 0x%"PRIx64" is a Miss.  Ignoring.  Most likely, we just recently wrote the data back anyway.\n", ev->getAddr());
            /* TODO:  May need to request upstream of us */
            delete ev;
        } else if ( src != SNOOP || ev->getDst() == getName() ) {

			if ( firstProcess) {
                listener->notifyAccess(CacheListener::READ, CacheListener::MISS, ev->getAddr());
                num_supply_miss++;
            }
			loadBlock(ev, src);
		} else {
            /* Ignore  - Snoop request, or not to us */
            delete ev;
        }
	}
}


void Cache::supplyData(MemEvent *ev, CacheBlock *block, SourceType_t src)
{

	supplyMap_t::key_type supplyMapKey = std::make_pair(block->baseAddr, src);
	supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
	ASSERT(supMapI != supplyInProgress.end());

    block->unlock();

	if ( supMapI->second.canceled ) {
		DPRINTF("Request has been canceled!\n");
		supplyInProgress.erase(supMapI);
        delete ev;
		return;
	}

	MemEvent *resp = new MemEvent(this, block->baseAddr, SupplyData);
    if ( block->user_locked ) {
        block->user_lock_needs_wb = true;
        resp->setFlag(MemEvent::F_DELAYED);
        resp->setSize(blocksize);
    } else {
        if ( block->status == CacheBlock::EXCLUSIVE )
            ev->setFlag(MemEvent::F_WRITEBACK);
        resp->setPayload(block->data);
    }


	switch (src) {
	case DOWNSTREAM:
		downstream_link->send(resp);
        supplyInProgress.erase(supMapI);
        if ( !resp->queryFlag(MemEvent::F_DELAYED) )
            block->status = CacheBlock::SHARED;
        break;
	case SNOOP: {
		BusHandlerArgs args;
		args.supplyData.block = block;
		args.supplyData.src = src;
        args.supplyData.isFakeSupply = resp->queryFlag(MemEvent::F_DELAYED);
		supMapI->second.busEvent = resp;
		DPRINTF("Enqueuing request to supply%s block 0x%"PRIx64"\n",
                args.supplyData.isFakeSupply ? " delay" : "",
                block->baseAddr);
		snoopBusQueue.request( resp,
				new BusFinishHandler(&Cache::finishBusSupplyData, args),
                new BusInitHandler(&Cache::prepBusSupplyData, args));
        break;
    }
	case DIRECTORY:
		directory_link->send(resp);
        supplyInProgress.erase(supMapI);
        assert( !resp->queryFlag(MemEvent::F_DELAYED) );
        block->status = CacheBlock::SHARED;
        break;
	case UPSTREAM:
		upstream_links[upstreamLinkMap[ev->getLinkId()]]->send(resp);
        supplyInProgress.erase(supMapI);
        break;
    default:
        break;
    }
	delete ev;
}

void Cache::prepBusSupplyData(BusHandlerArgs &args, MemEvent *ev)
{
    ev->setPayload(args.supplyData.block->data);
}


void Cache::finishBusSupplyData(BusHandlerArgs &args)
{
	DPRINTF("Supply Message sent for block 0x%"PRIx64"\n", args.supplyData.block->baseAddr);
	if ( !args.supplyData.isFakeSupply )
        args.supplyData.block->status = CacheBlock::SHARED;

	supplyMap_t::key_type supplyMapKey = std::make_pair(args.supplyData.block->baseAddr, args.supplyData.src);

	supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
	ASSERT(supMapI != supplyInProgress.end());
	supplyInProgress.erase(supMapI);
}



void Cache::handleCacheSupplyEvent(MemEvent *ev, SourceType_t src)
{
	if ( src == SNOOP && ev->getSrc() == getName() ) {
        delete ev;
        return; // We sent it
    }

	/*
	 * If snoop, cancel any supplies we're trying to do
	 * Check to see if we're trying to load this data, if so, handle
	 */
	if ( src == SNOOP ) {
		if ( ev->getSize() >= blocksize ) {
			Addr blkAddr = addrToBlockAddr(ev->getAddr());
			while ( blkAddr < (ev->getAddr() + ev->getSize()) ) {
                CacheBlock *b = findBlock(blkAddr);
                assert ( !b || b->status != CacheBlock::EXCLUSIVE );
				supplyMap_t::key_type supplyMapKey = std::make_pair(blkAddr, src);
				supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
				if ( supMapI != supplyInProgress.end() ) {
					// Mark it canceled
					DPRINTF("Marking request for 0x%"PRIx64" as canceled\n", ev->getAddr());
					supMapI->second.canceled = true;
					if ( supMapI->second.busEvent != NULL ) {
						// Bus requested.  Cancel it, too
						DPRINTF("Canceling Bus Request for Supply on 0x%"PRIx64" (%p)\n", supMapI->second.busEvent->getAddr(), supMapI->second.busEvent);
						BusHandlers handlers = snoopBusQueue.cancelRequest(supMapI->second.busEvent);
						if ( handlers.finish ) {
                            delete supMapI->second.busEvent;
							supMapI->second.busEvent = NULL;
							delete handlers.finish;
						}
                        if ( handlers.init ) delete handlers.init;
					}
				}
				blkAddr += blocksize;
			}
		}
	}


	/* Check to see if we're trying to load this data */
    LoadList_t::iterator i = waitingLoads.find(ev->getAddr());
    if ( i != waitingLoads.end() ) {
        LoadInfo_t* li = i->second;
        DPRINTF("We were waiting for block 0x%"PRIx64".  Processing.  [li: %p]\n", ev->getAddr(), li);

        if ( li->busEvent ) {
            DPRINTF("Canceling Bus Request for Load on 0x%"PRIx64"\n", li->busEvent->getAddr());
            BusHandlers handlers = snoopBusQueue.cancelRequest(li->busEvent);
            if ( handlers.init ) delete handlers.init;
            if ( handlers.finish ) delete handlers.finish;
            delete li->busEvent;
            li->busEvent = NULL;
        }

        if ( !li->targetBlock ) {
            DPRINTF("No block available yet.  We didn't ask for it.  Ignoring.\n");
            /* We still don't have a block assigned, so we didn't ask for
             * this.  Must be a snoop that we can ignore.
             * (no room in the inn) */
            ASSERT( src == SNOOP );
        } else {

            CacheBlock *targetBlock = li->targetBlock;

            if ( ev->queryFlag(MemEvent::F_DELAYED) ) {
                /* If we're trying to load this, but this is locked elsewhere, we need to wait longer for the data. */
                DPRINTF("Got a DELAYED Response.  Purge snoop work.\n");
                uint32_t deleted = 0;
                for ( uint32_t n = 0 ; n < li->list.size() ; n++ ) {
                    LoadInfo_t::LoadElement_t &oldEV = li->list[n];
                    if ( src == SNOOP && oldEV.src == SNOOP ) {
                        delete oldEV.ev;
                        oldEV.ev = NULL;
                        deleted++;
                    }
                }
                if ( deleted == li->list.size() ) {
                    /* Deleted all reasons to load this block */
                    waitingLoads.erase(i);
                    li->targetBlock->loadInfo = NULL;
                    delete li;
                    if ( targetBlock->isAssigned() ) targetBlock->status = CacheBlock::INVALID;
                    targetBlock->unlock();
                }

            } else {
                updateBlock(ev, li->targetBlock);
                li->targetBlock->loadInfo = NULL;
                li->targetBlock->status = CacheBlock::SHARED;
                li->targetBlock->unlock();

                for ( uint32_t n = 0 ; n < li->list.size() ; n++ ) {
                    LoadInfo_t::LoadElement_t &oldEV = li->list[n];
                    /* If this was from the Snoop Bus, and we've got other cache's asking
                     * for this data over the snoop bus, we can assume they saw it, and
                     * we don't need to reprocess them.
                     */
                    if ( src == SNOOP && oldEV.src == SNOOP ) {
                        delete oldEV.ev;
                    } else {
                        if ( oldEV.ev != NULL ) // handled before?
                            handleIncomingEvent(oldEV.ev, oldEV.src, false, true);
                    }
                }
                waitingLoads.erase(i);
                delete li;
            }

            handlePendingEvents(findRow(targetBlock->baseAddr), targetBlock);
        }

    } else {
        if ( cacheMode == INCLUSIVE ) {
            /* Not waiting for this load, and we're INCLUSIVE -> Must be a writeback. */
            CacheBlock *block = findBlock(ev->getAddr(), false);
            assert(block != NULL);
            assert(block->status == CacheBlock::DIRTY || src == SNOOP);
            updateBlock(ev, block);
            block->status = CacheBlock::SHARED;

        }
        if ( src == SNOOP ) {
            DPRINTF("No matching waitingLoads for 0x%"PRIx64".\n", ev->getAddr());
            if ( ev->getDst() == getName() ) {
                DPRINTF("WARNING:  Unmatched message.  Hopefully we recently just canceled this request, and our sender didn't get the memo.\n");
#if 0
                printCache();
                _abort(Cache, "%s Received an unmatched message!\n", getName().c_str());
#endif
            } else if ( downstream_link && ev->queryFlag(MemEvent::F_WRITEBACK) ) {
                // We're on snoop, but have a downstream, and this is a writeback.
                // We should probably pass it on.
                downstream_link->send(new MemEvent(ev));
            } else if ( directory_link && ev->queryFlag(MemEvent::F_WRITEBACK) ) {
                // Need to set src properly for the network.
                MemEvent *newev = new MemEvent(ev);
                newev->setSrc(getName());
                directory_link->send(newev);
            }
        } else if ( src == UPSTREAM ) {
            assert(ev->queryFlag(MemEvent::F_WRITEBACK));
            DPRINTF("Passing on writeback to next level\n");
            if ( downstream_link ) {
                downstream_link->send(new MemEvent(ev));
            } else if ( directory_link ) {
                // Need to set src properly for the network.
                MemEvent *newev = new MemEvent(ev);
                newev->setSrc(getName());
                directory_link->send(newev);
            } else {
                _abort(Cache, "Not sure where to send this.  Directory?\n");
            }
        } else {
            //_abort(Cache, "Unhandled case of unmatched SupplyData coming from non-SNOOP, non-UPSTREAM\n");
        }
    }

    delete ev;
}


void Cache::finishSupplyEvent(MemEvent *origEV, CacheBlock *block, SourceType_t origSrc)
{
    DPRINTF("\n");
	handleIncomingEvent(origEV, origSrc, false);
}


void Cache::handleInvalidate(MemEvent *ev, SourceType_t src, bool finishedUpstream)
{
	if ( src == SNOOP && ev->getSrc() == getName() ) {
        ackInvalidate(ev);
        return;
    }

	CacheBlock *block = findBlock(ev->getAddr());
    if ( block ) {
        if ( waitingForInvalidate(block->baseAddr) ) {
            //printf("%s: Recevied another invalidate for 0x%"PRIx64" from %s.  Canceling existing.\n", getName().c_str(), ev->getAddr(), ev->getSrc().c_str()); fflush(stdout);
            bool ok = cancelInvalidate(block); /* Should cause a re-issue of the write */
            if ( !ok ) {
                /* This was an un-cancelable invalidate that is in progress.
                 * Let's just retry later
                 */
                self_link->send(2, new SelfEvent(this, &Cache::retryEvent, ev, NULL, src));
                return;
            }
        }
    }


    if ( !finishedUpstream && (src == DOWNSTREAM || src == DIRECTORY) && !isL1 ) {
        //printf("Forwarding invalidate 0x%"PRIx64" on upstream.\n", ev->getAddr()); fflush(stdout);
        DPRINTF("Forwarding invalidate 0x%"PRIx64" on upstream.\n", ev->getAddr());
        issueInvalidate(ev, src, ev->getAddr(), SEND_UP, false);
        return;
    }

    if ( !finishedUpstream && (src == UPSTREAM) ) {
        //printf("Forwarding invalidate 0x%"PRIx64" downstream\n", ev->getAddr()); fflush(stdout);
        DPRINTF("Forwarding invalidate 0x%"PRIx64" downstream\n", ev->getAddr());
        issueInvalidate(ev, src, ev->getAddr(), SEND_DOWN, true);
        return;
    }

    if ( block ) {
        if ( block->status == CacheBlock::SHARED ) {
            DPRINTF("Invalidating block 0x%"PRIx64"\n", block->baseAddr);
            /* If we're trying to supply this block, cancel that. */

            supplyMap_t::key_type supplyMapKey = std::make_pair(block->baseAddr, SNOOP);
            supplyMap_t::iterator supMapI = supplyInProgress.find(supplyMapKey);
            if ( supMapI != supplyInProgress.end() ) {
                supMapI->second.canceled = true;
                if ( supMapI->second.busEvent != NULL ) {
                    // Bus requested.  Cancel it, too
                    DPRINTF("Canceling Bus Request for Supply on 0x%"PRIx64" (%p)\n", supMapI->second.busEvent->getAddr(), supMapI->second.busEvent);
                    BusHandlers handlers = snoopBusQueue.cancelRequest(supMapI->second.busEvent);
                    if ( handlers.finish ) {
                        delete supMapI->second.busEvent;
                        supMapI->second.busEvent = NULL;
                        delete handlers.finish;
                    }
                    if ( handlers.init ) delete handlers.init;
                }
            }


            if ( (cacheMode == INCLUSIVE) && (src != DOWNSTREAM) && (src != DIRECTORY) ) {
                block->status = CacheBlock::DIRTY;
                DPRINTF("Marking block 0x%"PRIx64" as DIRTY.\n", block->baseAddr);
            } else {
                block->status = CacheBlock::INVALID;
            }
            /* TODO:  Lock status? */
            handlePendingEvents(findRow(block->baseAddr), NULL);
        }
        if ( block->status == CacheBlock::EXCLUSIVE ) {
            DPRINTF("Invalidating EXCLUSIVE block 0x%"PRIx64" -> Issue writeback, pend invalidate\n", block->baseAddr);
            CacheRow *row = findRow(block->baseAddr);
            row->addWaitingEvent(ev, src);
            writebackBlock(block, CacheBlock::INVALID);
            return;
        }

    }

    if ( src != SNOOP ) {
        /* We'll need to ACK this */
        sendInvalidateACK(ev, src);
    }
    delete ev;
}


void Cache::sendInvalidateACK(MemEvent *ev, SourceType_t src)
{
    MemEvent *resp = ev->makeResponse(this);
    DPRINTF("Sending ACK for %s 0x%"PRIx64" to %s\n", CommandString[ev->getCmd()], ev->getAddr(), resp->getDst().c_str());
    switch (src) {
    case SNOOP:
        _abort(Cache, "We don't ACK on SNOOPY!\n");
        break;
    case UPSTREAM:
        upstream_links[upstreamLinkMap[ev->getLinkId()]]->send(resp);
        break;
    case DOWNSTREAM:
        downstream_link->send(resp);
        break;
    case DIRECTORY:
        directory_link->send(resp);
        break;
    case SELF:
        _abort(Cache, "Why are we acking to ourselfs?\n");
        break;
    case PREFETCHER:
        _abort(Cache, "Check this:  Sending Invalidate ACK to the prefetcher?\n");
        break;
    }
}


bool Cache::waitingForInvalidate(Addr addr)
{
    std::map<Addr, Invalidation>::iterator i = invalidations.find(addr);
	return ( i != invalidations.end() );
}

bool Cache::cancelInvalidate(CacheBlock *block)
{
    std::map<Addr, Invalidation>::iterator i = invalidations.find(block->baseAddr);
    assert(i != invalidations.end() );

    if ( i->second.canCancel ) {
        DPRINTF("Attempting cancel for Invalidate 0x%"PRIx64"\n", block->baseAddr);

        snoopBusQueue.cancelRequest(i->second.busEvent);

        std::deque<std::pair<MemEvent*, SourceType_t> > waitingEvents = i->second.waitingEvents;
        /* Only unlock if we locked it before */
        if ( i->second.block == block ) block->unlock();
        /* Erase before processing, otherwise, we'll think we're still waiting for invalidations */
        invalidations.erase(i);
    //printf("%s: Canceled Invalidation (%lu, %d) for 0x%"PRIx64"\n", getName().c_str(), i->second.issuingEvent.first, i->second.issuingEvent.second, i->first); fflush(stdout);

        DPRINTF("Due to cancel of Invalidate 0x%"PRIx64", re-issuing %zu events.\n", block->baseAddr, waitingEvents.size());
        while ( waitingEvents.size() > 0 ) {
            MemEvent *origEV = waitingEvents.front().first;
            SourceType_t origSRC = waitingEvents.front().second;
            waitingEvents.pop_front();
            self_link->send(1, new SelfEvent(this, &Cache::retryEvent, origEV, NULL, origSRC));
        }
        return true;
    } else {
        DPRINTF("Cannot cancel Invalidate 0x%"PRIx64"\n", block->baseAddr);
        return false;
    }
}


void Cache::ackInvalidate(MemEvent *ev)
{
    Addr addr = ev->getAddr();

    if ( (ev->getResponseToID() == invalidations[addr].issuingEvent) ||
            (ev->getSrc() == getName()) /* Self event on bus */) {
        int remaining = --invalidations[addr].waitingACKs;
        DPRINTF("Acknoweldging an Invalidate.  [%d remain]\n", remaining);

        ASSERT(remaining >= 0);
        if ( remaining == 0 )
            finishIssueInvalidate(addr);
    } else {
        DPRINTF("We aren't waiting for this ACK.  Ignore.\n");
    }

    delete ev;
}


void Cache::writebackBlock(CacheBlock *block, CacheBlock::BlockStatus newStatus)
{
    if ( block->wb_in_progress ) {
		DPRINTF("Writeback already in progress for block 0x%"PRIx64"\n", block->baseAddr);
        return;
    }
    block->wb_in_progress = true;
	if ( snoop_link ) {
		block->lock();
		BusHandlerArgs args;
		args.writebackBlock.block = block;
		args.writebackBlock.newStatus = newStatus;
		args.writebackBlock.decrementLock = true;
		DPRINTF("Enqueuing request to writeback block 0x%"PRIx64"\n", block->baseAddr);

        MemEvent *ev = new MemEvent(this, block->baseAddr, SupplyData);
        ev->setFlag(MemEvent::F_WRITEBACK);
        ev->setPayload(block->data);
		snoopBusQueue.request(ev,
				new BusFinishHandler(&Cache::finishWritebackBlockVA, args),
                new BusInitHandler(&Cache::prepWritebackBlock, args));
	} else {
		finishWritebackBlock(block, newStatus, false);
	}
}


void Cache::prepWritebackBlock(BusHandlerArgs &args, MemEvent *ev)
{
    ev->setPayload(args.writebackBlock.block->data);
}


void Cache::finishWritebackBlockVA(BusHandlerArgs& args)
{
	CacheBlock *block = args.writebackBlock.block;
	CacheBlock::BlockStatus newStatus = args.writebackBlock.newStatus;
	bool decrementLock = args.writebackBlock.decrementLock;
	finishWritebackBlock(block, newStatus, decrementLock);
}

void Cache::finishWritebackBlock(CacheBlock *block, CacheBlock::BlockStatus newStatus, bool decrementLock)
{

    block->wb_in_progress = false;
	if ( decrementLock ) /* AKA, sent on the Snoop Bus */
		block->unlock();

    /* TODO:  Verify that block->data can't change between snoop write and now */

	if ( downstream_link ) {
        MemEvent *ev = new MemEvent(this, block->baseAddr, SupplyData);
        ev->setFlag(MemEvent::F_WRITEBACK);
        ev->setPayload(block->data);
		downstream_link->send(ev);
    }
	if ( directory_link ) {
        MemEvent *ev = new MemEvent(this, block->baseAddr, SupplyData);
        ev->setFlag(MemEvent::F_WRITEBACK);
        ev->setPayload(block->data);
		directory_link->send(ev);
    }


    DPRINTF("Wrote Back Block 0x%"PRIx64"\tNew Status: %d\n", block->baseAddr, newStatus);

    CacheRow *row = findRow(block->baseAddr);
	block->status = newStatus;

    if ( newStatus == CacheBlock::INVALID ) {
        ASSERT(!block->isLocked());
        block = NULL;
    }

    handlePendingEvents(row, block);
}





void Cache::handleFetch(MemEvent *ev, bool invalidate, bool hasInvalidated)
{
    CacheBlock *block = findBlock(ev->getAddr(), false);
    /* Fetches should only come from directory controllers that know we have the block */
    assert(directory_link);
    if ( !block ) {
        DPRINTF("We were asked for 0x%"PRIx64", but we don't have it.  Punting.  Hope we recently did a return of it.\n", ev->getAddr());
        delete ev;
        return;
    }

    DPRINTF("0x%"PRIx64" block status: %d\n", block->baseAddr, block->status);

    if ( invalidate && !hasInvalidated ) {
        DPRINTF("Issuing invalidation for 0x%"PRIx64" upstream.\n", block->baseAddr);
        issueInvalidate(ev, DIRECTORY, block, CacheBlock::SHARED, SEND_UP);
        return;
    }

    switch ( block->status ) {
    case CacheBlock::SHARED: {
        MemEvent *me = ev->makeResponse(this);
        me->setDst(ev->getSrc());
        me->setPayload(block->data);
        directory_link->send(me);
        delete ev;
        break;
    }
    case CacheBlock::DIRTY:
        fetchBlock(ev, block);
        return; // Can't invalidate yet.
        break;
    default:
        _abort(Cache, "%d Not a legal status in a Fetch situation.\n", block->status);
    }

    if ( invalidate ) {
        block->status = CacheBlock::INVALID;
    }

}


/* Fetch a dirty block from upstream */
void Cache::fetchBlock(MemEvent *ev, CacheBlock *block)
{
    std::pair<LoadInfo_t*, bool> initRes = initLoad(ev, DIRECTORY);
    LoadInfo_t *li = initRes.first;
    bool reprocess = !initRes.second;

    li->targetBlock = block;
    li->loadDirection = SEND_UP;
    block->loadInfo = li;
    block->lock();

    if ( reprocess )
        li->list.push_front(LoadInfo_t::LoadElement_t(ev, DIRECTORY, getCurrentSimTime()));
    else
        li->list.push_back(LoadInfo_t::LoadElement_t(ev, DIRECTORY, getCurrentSimTime()));

    self_link->send(1, new SelfEvent(this, &Cache::finishLoadBlock, li, block->baseAddr, block));
}


void Cache::handleNACK(MemEvent *ev, SourceType_t src)
{
    /* Check the various queues to see what was NACK'd
     * Invalidations
     * Waiting Loads
     */
    std::map<Addr, Invalidation>::iterator i = invalidations.find(ev->getAddr()); if ( i != invalidations.end() ) {
        DPRINTF("NACK for Invalidation of 0x%"PRIx64"\n", ev->getAddr());
        if (ev->getResponseToID() == i->second.issuingEvent) {
            assert(i->second.canCancel);
            std::deque<std::pair<MemEvent*, SourceType_t> > waitingEvents = i->second.waitingEvents;
            invalidations.erase(i);
            while ( waitingEvents.size() > 0 ) {
                std::pair<MemEvent*, SourceType_t> oldEV = waitingEvents.front();
                waitingEvents.pop_front();

                if ( isL1 ) {
                    /* Can't propagate the NACK any higher, just re-issue */
                    CacheBlock *block = findBlock(ev->getAddr(), false);
                    if ( block && block->isLocked() ) block->unlock();
                    DPRINTF("Rescheduling event (%"PRIu64", %d) %s 0x%"PRIx64".\n",
                            oldEV.first->getID().first, oldEV.first->getID().second,
                            CommandString[oldEV.first->getCmd()], oldEV.first->getAddr());
                    self_link->send(1, new SelfEvent(this, &Cache::retryEvent, oldEV.first, NULL, oldEV.second));
                } else {
                    MemEvent *nack = oldEV.first->makeResponse(this);
                    nack->setCmd(NACK);
                    nack->setSize(0);
                    switch ( oldEV.second ) {
                    case  SNOOP:
                        snoopBusQueue.request(nack);
                        break;
                    case UPSTREAM:
                        upstream_links[upstreamLinkMap[oldEV.first->getLinkId()]]->send(nack);
                        break;
                    case DOWNSTREAM:
                        downstream_link->send(nack);
                        break;
                    case DIRECTORY:
                        directory_link->send(nack);
                        break;
                    case SELF:
                        _abort(Cache, "Shouldn't happen... NACK'ing an event we sent ourself?\n");
                        break;
                    case PREFETCHER:
                        _abort(Cache, "Check this:  Trying to send NACK to PREFETCHER.\n");
                        break;
                    }
                    delete oldEV.first;
                }
            }
            delete ev;
            return;
        } else {
            DPRINTF("NACK for Invalidation of 0x%"PRIx64" does not match request id.  Passing.\n", ev->getAddr());
        }
    }


	LoadList_t::iterator li = waitingLoads.find(ev->getAddr());
    if ( li != waitingLoads.end() ) {
        DPRINTF("NACK for RequestData of 0x%"PRIx64"\n", ev->getAddr());
        CacheBlock *block = li->second->targetBlock;
        //block->unlock();
        self_link->send(1, new SelfEvent(this, &Cache::finishLoadBlock, li->second, block->baseAddr, block));
        delete ev;
        return;
    }

    DPRINTF("Unexpected NACK for 0x%"PRIx64" received.  Ignoring.\n", ev->getAddr());

    delete ev;
}


/**** Utility Functions ****/

void Cache::handlePendingEvents(CacheRow *row, CacheBlock *block)
{
    if ( row->waitingEvents.size() > 0 ) {

        std::map<Addr, CacheRow::eventQueue_t>::iterator q;
        if ( block ) {
            q = row->waitingEvents.find(block->baseAddr);
        } else {
            /* Pick one. */
            q = row->waitingEvents.begin();
        }
        if ( q != row->waitingEvents.end() ) {
            CacheRow::eventQueue_t &queue = q->second;
            while ( queue.size() ) {
                std::pair<MemEvent*, SourceType_t> ev = queue.front();
                queue.pop_front();
                DPRINTF("Issuing Retry for event (%"PRIu64", %d) %s [0x%"PRIx64"]\n",
                        ev.first->getID().first, ev.first->getID().second, CommandString[ev.first->getCmd()], ev.first->getAddr());
                self_link->send(1, new SelfEvent(this, &Cache::retryEvent, ev.first, NULL, ev.second));
            }
            row->waitingEvents.erase(q);
        }
    }

}


void Cache::updateBlock(MemEvent *ev, CacheBlock *block)
{
	if (ev->getSize() == blocksize) {
		// Assumption:  if sizes are equal, base addresses are, too
		std::copy(ev->getPayload().begin(), ev->getPayload().end(), block->data.begin());
	} else {
		// Update a portion of the block
		Addr blockoffset = (ev->getAddr() <= block->baseAddr) ?
			0 :
			ev->getAddr() - block->baseAddr;

		Addr payloadoffset = (ev->getAddr() >= block->baseAddr) ?
			0 :
			block->baseAddr - ev->getAddr();

		for ( uint32_t i = 0 ; i < std::min(blocksize,ev->getSize()) ; i++ ) {
			ASSERT(blockoffset+i < blocksize);
			block->data[blockoffset+i] = ev->getPayload()[payloadoffset+i];
		}
	}
#if 0
    char buffer[2*64 + 8] = {0};
    char *b = buffer;
    b += sprintf(b, " [0x");
    for ( uint32_t i = 0 ; i < blocksize ; i++ ) {
        b += sprintf(b, "%02x", block->data[i]);
    }
    b += sprintf(b, "]");
    DPRINTF("Updating block 0x%"PRIx64" %s\n", block->baseAddr, buffer);
#else
    DPRINTF("Updating block 0x%"PRIx64"\n", block->baseAddr);
#endif
	block->last_touched = getCurrentSimTime();
}



int Cache::numBits(int x)
{
	return (int)(log(x)/log(2));
}

Addr Cache::addrToTag(Addr addr)
{
	return (addr >> tagshift);
}

Addr Cache::addrToBlockAddr(Addr addr)
{
	return addr & ~(blocksize-1);
}

Cache::CacheBlock* Cache::findBlock(Addr addr, bool emptyOK)
{
	CacheBlock *block = NULL;
	CacheRow *row = findRow(addr);
	uint32_t tag = addrToTag(addr);
	for ( int i = 0 ; i < n_ways ; i++ ) {
		CacheBlock *b = &(row->blocks[i]);
		if ( b->isValid() && b->tag == tag ) {
			block = b;
			break;
		}
	}
	if ( ! block && emptyOK ) { // See if we can find an empty
		for ( int i = 0 ; i < n_ways ; i++ ) {
			CacheBlock *b = &(row->blocks[i]);
			if ( b->isInvalid() ) {
				block = b;
				break;
			}
		}
	}
	return block;
}


Cache::CacheRow* Cache::findRow(Addr addr)
{
	/* Calculate Row number */
	Addr row = (addr >> rowshift) & rowmask;
	ASSERT(row < (Addr)n_rows);
	return &database[row];
}



std::string Cache::findTargetDirectory(Addr addr)
{
    for ( std::vector<MemNIC::ComponentInfo>::iterator i = directories.begin() ; i != directories.end() ; ++i ) {
        MemNIC::ComponentTypeInfo &di = i->typeInfo;
        if ( addr >= di.dirctrl.rangeStart && addr < di.dirctrl.rangeEnd ) {
            if ( di.dirctrl.interleaveSize == 0 ) {
                return i->name;
            } else {
                Addr temp = addr - di.dirctrl.rangeStart;
                Addr offset = temp % di.dirctrl.interleaveStep;
                if ( offset < di.dirctrl.interleaveSize ) {
                    return i->name;
                }
            }
        }
    }
    _abort(Cache, "Unable to find directory for address 0x%"PRIx64"\n", addr);
    return "";
}



void Cache::printCache(void)
{
	static const char *status[] = {"I", "A", "S", "E"};
	std::ostringstream ss;

	ss << getName() << std::endl;
	ss << std::setfill('0');

	for ( int r = 0 ; r < n_rows; r++ ) {
		ss << std::setw(2) << r << " | ";
		CacheRow &row = database[r];
		for ( int c = 0 ; c < n_ways ; c++ ) {
			CacheBlock &b = row.blocks[c];
			ss << status[b.status] << " ";
			ss << "0x" << std::setw(8) << std::hex << b.baseAddr << std::dec << " ";
			ss << std::setw(4) << b.tag << " ";
			ss << "| ";
		}
		ss << std::endl;
	}

	ss << std::endl;
    if ( waitingLoads.size() > 0 ) {
        ss << "Waiting Loads\n";
        for ( LoadList_t::iterator i = waitingLoads.begin() ; i != waitingLoads.end() ; ++i ) {
            Addr addr = i->first;
            LoadInfo_t* li = i->second;

            ss << "0x" << std::setw(4) << std::hex << addr << std::dec;
            if ( li->targetBlock != NULL )
                ss << " slated for [" << li->targetBlock->row << ", " << li->targetBlock->col << "]";
            ss << "\n";

            for ( std::deque<LoadInfo_t::LoadElement_t>::iterator j = li->list.begin() ; j != li->list.end() ; ++j ) {
                MemEvent *ev = j->ev;
                SimTime_t elapsed = getCurrentSimTime() - j->issueTime;
                ss << "\t(" << ev->getID().first << ", " << ev->getID().second << ")  " << CommandString[ev->getCmd()] << "\t" << elapsed << "\n";
            }
        }
    }

    size_t num_pend = 0;
    for ( int r = 0 ; r < n_rows ; r++ ) {
        CacheRow *row = &database[r];
        num_pend += row->waitingEvents.size();
    }
    if ( num_pend > 0 ) {
        ss << "Pending Events\t" << num_pend << std::endl;
        for ( int r = 0 ; r < n_rows ; r++ ) {
            CacheRow *row = &database[r];
            if ( row->waitingEvents.size() > 0 ) {
                ss << "Row " << r << "\n";
#if 0
                for ( std::deque<std::pair<MemEvent*, SourceType_t> >::iterator i = row->waitingEvents.begin() ; i != row->waitingEvents.end() ; ++i ) {
                    MemEvent *ev = i->first;
                    ss << "\tEvent id (" <<  ev->getID().first << ", " << ev->getID().second << ") Command:  " << CommandString[ev->getCmd()] << "  0x" << std::hex << ev->getAddr() << std::dec << "\n";
                }
#else
                for ( std::map<Addr, CacheRow::eventQueue_t>::iterator i = row->waitingEvents.begin () ; i != row->waitingEvents.end() ; ++i ) {
                    ss << "\tBlock Address    0x" << std::hex << i->first << std::dec << "\n";
                    for ( CacheRow::eventQueue_t::iterator j = i->second.begin() ; j != (i->second.end()) ; ++j ) {
                        MemEvent *ev = j->first;
                        ss << "\t\tEvent id (" <<  ev->getID().first << ", " << ev->getID().second << ") Command:  " << CommandString[ev->getCmd()] << "  0x" << std::hex << ev->getAddr() << std::dec << "\n";
                    }
                }
#endif
            }
        }
    }

    if ( snoopBusQueue.size() ) {
        ss << "Bus Queue Size:  " << snoopBusQueue.size() << "\n";
    }

	std::cout << ss.str();

}
