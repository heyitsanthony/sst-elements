// Copyright 2009-2017 Sandia Corporation. Under the terms
// of Contract DE-NA0003525 with Sandia Corporation, the U.S.
// Government retains certain rights in this software.
//
// Copyright (c) 2009-2017, Sandia Corporation
// All rights reserved.
//
// Portions are copyright of other developers:
// See the file CONTRIBUTORS.TXT in the top level directory
// the distribution for more information.
//
// This file is part of the SST software package. For license
// information, see the LICENSE file in the top level directory of the
// distribution.

#ifndef _MEMHIERARCHY_CUSTOMMEMEVENT_H_
#define _MEMHIERARCHY_CUSTOMMEMEVENT_H_

#include <sst/core/sst_types.h>
#include "util.h"
#include "memEventBase.h"
#include "memTypes.h"

namespace SST { namespace MemHierarchy {

using namespace std;

class CustomCmdEvent : public MemEventBase {
public:

    CustomCmdEvent(std::string src, Addr addr, Command cmd, uint32_t opc = 0) : 
        MemEventBase(src, cmd), addr_(addr), addrGlobal_(true), opCode_(opc), instPtr_(0), vAddr_(0) { }

    /* Getters/setters */
    void setAddr(Addr addr) { addr_ = addr; }
    Addr getAddr() { return addr_; }

    void setAddrGlobal(bool val) { addrGlobal_ = val; }
    bool isAddrGlobal() { return addrGlobal_; }

    void setOpCode(uint32_t opc) { opCode_ = opc; }
    uint32_t getOpCode() { return opCode_; }

    void setInstructionPointer(Addr ip) { instPtr_ = ip; }
    Addr getInstructionPointer() { return instPtr_; }

    void setVirtualAddress(Addr va) { vAddr_ = va; }
    Addr getVirtualAddress() { return vAddr_; }

    /* Virtual functions inherited from MemEventBase */
    virtual CustomCmdEvent* makeResponse() override {
        CustomCmdEvent* me = new CustomCmdEvent(*this);
        me->setResponse(this);
        me->instPtr_ = instPtr_;
        me->vAddr_ = vAddr_;
        me->opCode_ = opCode_;
        return me;
    }

    virtual uint32_t getEventSize() override { return 0; }

    virtual std::string getVerboseString() override {
        std::ostringstream str;
        str << std::hex << " Addr: 0x" << addr_;
        str << (addrGlobal_ ? "(Global)" : "(Local)");
        str << " VA: 0x" << vAddr_ << " IP: 0x" << instPtr_;
        str << " OpCode: 0x" << opCode_;
        return MemEventBase::getVerboseString() + str.str();
    }

    virtual std::string getBriefString() {
        std::ostringstream str;
        str << std::hex << " Addr: 0x" << addr_;
        str << " OpCode: 0x" << opCode_;
        return MemEventBase::getBriefString() + str.str();
    }

    virtual bool doDebug(std::set<Addr> &addr) {
        return (addr.find(addr_) != addr.end());
    }

    virtual Addr getRoutingAddress() {
        return addr_;
    }

    virtual CustomCmdEvent* clone(void) override {
        return new CustomCmdEvent(*this);
    }

private:
    Addr                    addr_;      /* TODO is baseAddr needed? */
    bool                    addrGlobal_;/* Is address global or local? */
    uint32_t                opCode_;    /* Custom Op Code */
    Addr                    instPtr_;   /* Instruction pointer */
    Addr                    vAddr_;     /* Virtual address */

    CustomCmdEvent() : MemEventBase() { } // For serialization only

/* Serialization */
public:
    void serialize_order(SST::Core::Serialization::serializer &ser) override {
        MemEventBase::serialize_order(ser);
        ser & addr_;
        ser & addrGlobal_;
        ser & opCode_;
        ser & instPtr_;
        ser & vAddr_;
    }

    ImplementSerializable(SST::MemHierarchy::CustomCmdEvent);

};

}}

#endif
