/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include "jlib.hpp"
#include "workunit.hpp"
#include "jptree.hpp"
#include "jlog.hpp"
#include "jregexp.hpp"
#include "workflow.hpp"
#include "dasds.hpp"

//------------------------------------------------------------------------------------------
/* Parallel Workflow explanation

Key information
•	All items are executed a maximum of one time per query. (Unless they are recovered)
•	ECL actions are in one-to-one correspondence with the workflow items that houses the action.

Ready criteria
The core criteria defining when items are added to the task queue is if they have any unperformed dependencies. A second criteria is whether the item is active or inactive. See below

General process
•	The program checks workflow items against a blacklist to determine if parallel execution is supported.
•	The program recursively traces through each item’s dependencies, constructing the “graph of successors”. See below
•	Items without dependencies are placed on the starting task queue
•	Threads are created
•	Threads perform the thread specific process
•	Threads finish, once all items have been executed

Thread specific process
•	Wait for an item to be added to the task queue
•	Pop an item from the task queue
•	Execute item
•	Alert successors to the item (convey that one of their dependencies has been completed)
•	Add any successors to the task queue if they meet the "ready criteria"

What is the graph of successors?
The relationship between an item and its dependency is two-directional.
•	The dependency must be done first
•	The item should be done second. Therefore, the item is a successor to the dependency.
The term graph represents the idea that the successor of an item may also have its own successors. (You could sketch this out visually)
This is a consequence of allowing an item's dependency to also have its own dependencies.
An ECL query always generates at least one item with zero successors, called the "parent item”. This is the item to execute last (unless the query fails)

Dependent successors are those described above; they are the reverse of a dependency.
Logical successors are a second type of successor. Logical successors have no dependency on their predecessor, yet must execute afterwards.
Logical successors are used for a variety of reasons involving ORDERED, SEQUENTIAL, IF, SUCCESS/FAILURE.

There are scenarios where an item will execute before its logical predecessor. (But this can't happen for a dependent successor)
i.e. the ECL code: PARALLEL(b, ORDERED(a,b))
This may cause action b to be executed before a - even though there is a logical successorship from a to b due to ORDERED.
You could say that this logical successorship is made obsolete by the encompassing PARALLEL statement.
This code example shows that although logical successorships are added in the graph of successors, they may never be used.

PARALLEL, ORDERED and SEQUENTIAL are ways to group actions and to specify if they have any ordering requirements. (An ordering requirement could be: action 1 needs to be performed before action 2)
I will describe how they work in terms of the first and second actions in the actionlist, without any loss of generality.
The relationship from the second action to the first is *exactly* the same as the relationship from the third action to the second, and so on.

PARALLEL
The actions in a parallel actionlist have no special ordering requirements. Any actions or their dependencies can be performed concurrently.
In relation to the workflow engine, there are no logical dependencies between the actions or their dependencies

SEQUENTIAL
The actions in a sequential actionlist have the most constrained ordering requirements. Firstly, the actions must be performed in order. Secondly, any dependencies to an action can only be started once the previous action has finished.
In relation to the workflow engine, the second action in the actionlist has a logical dependency on the first action. Furthermore, each of the second action's dependencies has a logical dependency on the first action.

ORDERED
The actions in an ordered actionlist have a less constrained ordering requirement than sequential. Only the actions in the actionlist must be performed in order, but there is no special ordering requirement for their dependencies.
In relation to the workflow engine, the second action in the actionlist has a logical dependency on the first action. This is not true of the second action's dependencies, which can be executed at any point.

What makes an item active?
Any actions explicitly called in the ECL query are active. For the query to succeed, it is always necessary for these item to execute.
Any items that these actions depend on also get marked as active.
Items that start without being active (i.e. those that might not execute during the workunit) are:
•	items that are logical successors of other items, and not activated by another route
These could be:
•	items that are contingencies (SUCCESS/FAILURE)
•	items that are the trueresult or falseresult of a conditional IF function
If one of these items is going to execute, then they are marked as active.
For example, logical successorships (described above) entail a predecessor "activating" its successor.
An item is active when it has a reason to execute.

Complications
The algorithm is made more complicated by having to
•	co-ordinate when the worker threads stop
•	catch any failed workflow items and perform the failure contingencies
•	identify whether persist items (and dependencies) are up-to-date
•	protect shared resources such as the task queue from race conditions.
*/

EnumMapping wftypes[] =
{
    { WFTypeNormal, "normal" },
    { WFTypeSuccess, "success" },
    { WFTypeFailure, "failure" },
    { WFTypeRecovery, "recovery" },
    { WFTypeWait, "wait" },
    { WFTypeSize, NULL }
};

EnumMapping wfmodes[] =
{
    { WFModeNormal, "normal" },
    { WFModeCondition, "condition" },
    { WFModeSequential, "sequential" },
    { WFModeParallel, "parallel" },
    { WFModePersist, "persist" },
    { WFModeBeginWait, "bwait" },
    { WFModeWait, "wait" },
    { WFModeOnce, "once" },
    { WFModeCritical, "critical" },
    { WFModeOrdered, "ordered" },
    { WFModeConditionExpression, "condition expression" },
    { WFModePersistActivator, "persist activator" },
    { WFModeSize, NULL}
};

EnumMapping wfstates[] =
{
   { WFStateNull, "null" },
   { WFStateReqd, "reqd" },
   { WFStateDone, "done" },
   { WFStateFail, "fail" },
   { WFStateSkip, "skip" },
   { WFStateWait, "wait" },
   { WFStateBlocked, "block" },
   { WFStateSize, NULL }
};

static void setEnum(IPropertyTree *p, const char *propname, int value, EnumMapping *map)
{
    const char * mapped = getEnumText(value, map, nullptr);
    if (!mapped)
        assertex(!"Unexpected value in setEnum");
    p->setProp(propname, mapped);
}

static int getEnum(IPropertyTree *p, const char *propname, EnumMapping *map)
{
    const char *v = p->queryProp(propname);
    if (v)
        return getEnum(v, map);
    return 0;
}

const char * queryWorkflowTypeText(WFType type)
{
    return getEnumText(type, wftypes);
}

const char * queryWorkflowModeText(WFMode mode)
{
    return getEnumText(mode, wfmodes);
}

const char * queryWorkflowStateText(WFState state)
{
    return getEnumText(state, wfstates);
}


class CWorkflowDependencyIterator : implements IWorkflowDependencyIterator, public CInterface
{
public:
    CWorkflowDependencyIterator(IPropertyTree * tree) { iter.setown(tree->getElements("Dependency")); }
    IMPLEMENT_IINTERFACE;
    bool                first() { return iter->first(); }
    bool                isValid() { return iter->isValid(); }
    bool                next() { return iter->next(); }
    unsigned            query() const { return iter->query().getPropInt("@wfid"); }
private:
    Owned<IPropertyTreeIterator> iter;
};

class CWorkflowEvent : public CInterface, implements IWorkflowEvent
{
public:
    CWorkflowEvent(char const * _name, char const * _text) : name(_name), text(_text) {}
    IMPLEMENT_IINTERFACE;
    virtual char const * queryName() const { return name.get(); }
    virtual char const * queryText() const { return text.get(); }
    virtual bool matches(char const * trialName, char const * trialText) const { return((strcmp(trialName, name.get()) == 0) && WildMatch(trialText, text.get(), true)); }
private:
    StringAttr name;
    StringAttr text;
};

class CWorkflowItem : implements IWorkflowItem, public CInterface
{
public:
    CWorkflowItem(IPropertyTree & _tree) { tree.setown(&_tree); }
    CWorkflowItem(IPropertyTree * ptree, unsigned wfid, WFType type, WFMode mode, unsigned success, unsigned failure, unsigned recovery, unsigned retriesAllowed, unsigned contingencyFor)
    {
        tree.setown(LINK(ptree->addPropTree("Item")));
        tree->setPropInt("@wfid", wfid);
        setEnum(tree, "@type", type, wftypes);
        setEnum(tree, "@mode", mode, wfmodes);
        if(success) tree->setPropInt("@success", success);
        if(failure) tree->setPropInt("@failure", failure);
        if(recovery && retriesAllowed)
        {
            tree->setPropInt("@recovery", recovery);
            tree->setPropInt("@retriesAllowed", retriesAllowed);
            tree->addPropTree("Dependency")->setPropInt("@wfid", recovery);
        }
        if(contingencyFor) tree->setPropInt("@contingencyFor", contingencyFor);
        reset();
    }

    IMPLEMENT_IINTERFACE;
    //info set at compile time
    virtual unsigned     queryWfid() const { return tree->getPropInt("@wfid"); }
    virtual bool         isScheduled() const { return tree->hasProp("Schedule"); }
    virtual bool         isScheduledNow() const { return (tree->hasProp("Schedule") && !tree->hasProp("Schedule/Event")); }
    virtual IWorkflowEvent * getScheduleEvent() const { if(tree->hasProp("Schedule/Event")) return new CWorkflowEvent(tree->queryProp("Schedule/Event/@name"), tree->queryProp("Schedule/Event/@text")); else return NULL; }
    virtual unsigned     querySchedulePriority() const { return (tree->hasProp("Schedule") ? tree->getPropInt("Schedule/@priority", 0) : 0); }
    virtual bool         hasScheduleCount() const { return tree->hasProp("Schedule/@count"); }
    virtual unsigned     queryScheduleCount() const { assertex(tree->hasProp("Schedule/@count")); return tree->getPropInt("Schedule/@count"); }
    virtual IWorkflowDependencyIterator * getDependencies() const { return new CWorkflowDependencyIterator(tree); }
    virtual WFType       queryType() const { return static_cast<WFType>(getEnum(tree, "@type", wftypes)); }
    virtual IStringVal & getLabel(IStringVal & val) const { val.set(tree->queryProp("@label")); return val; }
    virtual WFMode       queryMode() const { return static_cast<WFMode>(getEnum(tree, "@mode", wfmodes)); }
    virtual unsigned     querySuccess() const { return tree->getPropInt("@success", 0); }
    virtual unsigned     queryFailure() const { return tree->getPropInt("@failure", 0); }
    virtual unsigned     queryRecovery() const { return tree->getPropInt("@recovery", 0); }
    virtual unsigned     queryRetriesAllowed() const { return tree->getPropInt("@retriesAllowed", 0); }
    virtual unsigned     queryContingencyFor() const { return tree->getPropInt("@contingencyFor", 0); }
    virtual IStringVal & getPersistName(IStringVal & val) const { val.set(tree->queryProp("@persistName")); return val; }
    virtual unsigned     queryPersistWfid() const { return tree->getPropInt("@persistWfid", 0); }
    virtual int          queryPersistCopies() const { return tree->getPropInt("@persistCopies", 0); }
    virtual bool         queryPersistRefresh() const { return tree->getPropBool("@persistRefresh", true); }
    virtual IStringVal & getCriticalName(IStringVal & val) const { val.set(tree->queryProp("@criticalName")); return val; }
    virtual IStringVal & queryCluster(IStringVal & val) const { val.set(tree->queryProp("@cluster")); return val; }
    virtual void         setScheduledNow() { tree->setPropTree("Schedule"); setEnum(tree, "@state", WFStateReqd, wfstates); }
    virtual void         setScheduledOn(char const * name, char const * text) { IPropertyTree * stree =  tree->setPropTree("Schedule")->setPropTree("Event"); stree->setProp("@name", name); stree->setProp("@text", text);; setEnum(tree, "@state", WFStateWait, wfstates); }
    virtual void         setSchedulePriority(unsigned priority) { assertex(tree->hasProp("Schedule")); tree->setPropInt("Schedule/@priority", priority); }
    virtual void         setScheduleCount(unsigned count) { assertex(tree->hasProp("Schedule")); tree->setPropInt("Schedule/@count", count); tree->setPropInt("Schedule/@countRemaining", count); }
    virtual void         addDependency(unsigned wfid) { tree->addPropTree("Dependency")->setPropInt("@wfid", wfid); }
    virtual void         setPersistInfo(char const * name, unsigned wfid, int numPersistInstances, bool refresh)
    {
        tree->setProp("@persistName", name);
        tree->setPropInt("@persistWfid", wfid);
        if (numPersistInstances != 0)
            tree->setPropInt("@persistCopies", (int)numPersistInstances);
        tree->setPropBool("@persistRefresh", refresh);
    }
    virtual void         setCriticalInfo(char const * name) { tree->setProp("@criticalName", name);}
    virtual void         setCluster(const char * cluster) { tree->setProp("@cluster", cluster); }
    //info set at run time
    virtual unsigned     queryScheduleCountRemaining() const { assertex(tree->hasProp("Schedule")); return tree->getPropInt("Schedule/@countRemaining"); }
    virtual WFState      queryState() const { return static_cast<WFState>(getEnum(tree, "@state", wfstates)); }
    virtual unsigned     queryRetriesRemaining() const { return tree->getPropInt("@retriesRemaining"); }
    virtual int          queryFailCode() const { return tree->getPropInt("@failcode"); }
    virtual char const * queryFailMessage() const { return tree->queryProp("@failmsg"); }
    virtual char const * queryEventName() const { return tree->queryProp("@eventname"); }
    virtual char const * queryEventExtra() const { return tree->queryProp("@eventextra"); }
    virtual void         setState(WFState state) { setEnum(tree, "@state", state, wfstates); }
    virtual unsigned     queryScheduledWfid() const { return tree->getPropInt("@swfid", 0); }
    virtual void         setScheduledWfid(unsigned wfid) { tree->setPropInt("@swfid", wfid); }
    virtual void         setLabel(const char * label) { tree->setProp("@label", label); }
    virtual bool         testAndDecRetries()
    {
        assertex(tree->hasProp("@retriesAllowed"));
        unsigned rem = tree->getPropInt("@retriesRemaining", 0);
        if(rem==0)
            return false;
        tree->setPropInt("@retriesRemaining", rem-1);
        return true;
    }
    virtual bool         decAndTestScheduleCountRemaining()
    {
        if(!tree->hasProp("Schedule/@count"))
            return true;
        unsigned rem = tree->getPropInt("Schedule/@countRemaining");
        assertex(rem>0);
        tree->setPropInt("Schedule/@countRemaining", rem-1);
        return (rem>1);
    }
    virtual void incScheduleCount()
    {
        unsigned rem = tree->getPropInt("Schedule/@countRemaining");
        tree->setPropInt("Schedule/@countRemaining", rem+1);
    }
    virtual void         setFailInfo(int code, char const * message)
    {
        tree->setPropInt("@failcode", code);
        tree->setProp("@failmsg", message);
    }
    virtual void         setEvent(const char * name, const char * extra)
    {
        if (name)
            tree->setProp("@eventname", name);
        if (extra)
            tree->setProp("@eventextra", extra);
    }
    virtual void         reset()
    {
        if(tree->hasProp("@retriesAllowed"))
            tree->setPropInt("@retriesRemaining", tree->getPropInt("@retriesAllowed"));
        if(tree->hasProp("Schedule/@count"))
            tree->setPropInt("Schedule/@countRemaining", tree->getPropInt("Schedule/@count"));
        tree->removeProp("@failcode");
        tree->removeProp("@failmsg");
        tree->removeProp("@eventname");
        tree->removeProp("@eventtext");
        if(isScheduled())
        {
            if(isScheduledNow())
                setState(WFStateReqd);
            else if (hasScheduleCount() && (queryScheduleCountRemaining() == 0))
                setState(WFStateDone);
            else
                setState(WFStateWait);
        }
        else if(queryType() == WFTypeRecovery)
            setState(WFStateSkip);
        else
            setState(WFStateNull);
    }
    virtual void         syncRuntimeData(IConstWorkflowItem const & other)
    {
        WFState state = other.queryState();
        setState(state);
        if(tree->hasProp("@retriesAllowed"))
            tree->setPropInt("@retriesRemaining", other.queryRetriesRemaining());
        if(tree->hasProp("Schedule/@count"))
            tree->setPropInt("Schedule/@countRemaining", other.queryScheduleCountRemaining());
        if(state == WFStateFail)
        {
            tree->setPropInt("@failcode", other.queryFailCode());
            tree->setProp("@failmsg", other.queryFailMessage());
        }
        setEvent(other.queryEventName(), other.queryEventExtra());
    }
private:
    Owned<IPropertyTree> tree;
};

class CCloneWorkflowItem : public CInterface, implements IRuntimeWorkflowItem
{
private:
    class CCloneSchedule : public CInterface
    {
    private:
        bool now;
        unsigned priority;
        bool counting;
        unsigned count;
        unsigned countRemaining;
        Owned<IWorkflowEvent> event;
    public:
        CCloneSchedule(IConstWorkflowItem const * other)
        {
            now = other->isScheduledNow();
            priority = other->querySchedulePriority();
            counting = other->hasScheduleCount();
            if(counting)
            {
                count = other->queryScheduleCount();
                countRemaining = other->queryScheduleCountRemaining();
            }
            else
            {
                count = 0;
                countRemaining = 0;
            }
            event.setown(other->getScheduleEvent());
        }
        bool isNow() const { return now; }
        unsigned queryPriority() const { return priority; }
        bool hasCount() const { return counting; }
        unsigned queryCount() const { return count; }
        unsigned queryCountRemaining() const { return countRemaining; }
        bool decAndTestCountRemaining()
        {
            if(!counting)
                return true;
            if(countRemaining)
                countRemaining--;
            return (countRemaining>0);
        }
        void incCountRemaining()
        {
            if(counting)
                countRemaining++;
        }
        void resetCount() { if(counting) countRemaining = count; }
        IWorkflowEvent * getEvent() const { return event.getLink(); }
    };

    class CCloneIterator : public CInterface, public IWorkflowDependencyIterator
    {
    public:
        CCloneIterator(IntArray const & _array) : array(_array), idx(0) {}
        IMPLEMENT_IINTERFACE;
        virtual bool first() { idx = 0; return isValid(); }
        virtual bool isValid() { return array.isItem(idx); }
        virtual bool next() { idx++; return isValid(); }
        virtual unsigned query() const { return array.item(idx); }
    private:
        IntArray const & array;
        aindex_t idx;
    };

    unsigned wfid;
    //If an item has an exception, only the failure contingency should execute
    Owned<WorkflowException> thisException;
    Owned<CCloneSchedule> schedule;
    IntArray dependencies;
    IntArray dependentSuccessors;
    //These are the items that are activated, upon completion (of this item)
    IntArray logicalSuccessors;
    //This is the number of unperformed dependencies belonging to the item. It is decreased until it reaches 0
    std::atomic<unsigned int> numDependencies{0U};
    //once the abort flag is set in WFMachine, only items marked with executeOnAbort will be processed
    std::atomic<bool> executeOnAbort{false};
    //An item will only be executed if it is active
    std::atomic<bool> active{false};
    //context in which an item has been added to the task queue. This catches contingency failures
    unsigned withinContingency = 0;
    WFType type = WFTypeNormal;
    WFMode mode = WFModeNormal;
    //wfid of other items
    unsigned success;
    unsigned failure;
    unsigned recovery;

    unsigned retriesAllowed;
    unsigned contingencyFor;
    unsigned scheduledWfid;
    WFState state = WFStateNull;
    unsigned retriesRemaining;
    int failcode;
    StringAttr failmsg;
    SCMStringBuffer persistName;
    SCMStringBuffer clusterName;
    SCMStringBuffer label;
    //wfid of the persist activator/successor (Each will point to the other)
    unsigned persistWfid;
    //pointer to persist activator in order to manage new logical successorships
    CCloneWorkflowItem * persistCounterpart;
    int persistCopies;
    bool persistRefresh = true;
    Owned<IRemoteConnection> persistLock;
    Owned<PersistVersion> thisPersist;
    SCMStringBuffer criticalName;
    StringAttr eventName;
    StringAttr eventExtra;

public:
    CCloneWorkflowItem(){}
    CCloneWorkflowItem(unsigned _wfid)
    {
        wfid = _wfid;
    }
    IMPLEMENT_IINTERFACE;
    void incNumDependencies()
    {
        numDependencies++;
    }
    unsigned atomicDecNumDependencies()
    {
        return numDependencies.fetch_sub(1);
    }
    unsigned queryNumDependencies() const { return numDependencies; }
    unsigned queryNumDependentSuccessors() const { return dependentSuccessors.ordinality(); }
    unsigned queryNumLogicalSuccessors() const { return logicalSuccessors.ordinality(); }
    bool isDependentSuccessorsEmpty() const
    {
        return dependentSuccessors.empty();
    }
    void addDependentSuccessor(CCloneWorkflowItem * next)
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Workflow item %u has marked workflow item %u as its successor", wfid, next->queryWfid());
#endif
        dependentSuccessors.append(next->queryWfid());
        next->incNumDependencies();
    }
    void addLogicalSuccessor(CCloneWorkflowItem * next)
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Workflow item %u has marked workflow item %u as its successor", wfid, next->queryWfid());
#endif
        //persistActivator is used as a logical predecessor to dependencies of persist items
        unsigned nextWfid;
        if(next->queryMode() == WFModePersist)
            logicalSuccessors.append(next->queryPersistWfid());
        logicalSuccessors.append(next->queryWfid());
        //note that dependency count is not incremented, since logical successors don't follow as dependents
        //Instead, logical relationships are used to activate the successors
    }
    //For condition expression
    void removeLogicalSuccessors()
    {
        if(logicalSuccessors.empty())
            throwUnexpected();
        logicalSuccessors.clear();
    }
    IWorkflowDependencyIterator * getDependentSuccessors() const
    {
        return new CCloneIterator(dependentSuccessors);
    }
    IWorkflowDependencyIterator * getLogicalSuccessors() const
    {
        return new CCloneIterator(logicalSuccessors);
    }
    void activate()
    {
        if (mode == WFModePersist)
        {
            if (!persistCounterpart->isActive())
                persistCounterpart->activate();
        }
        active = true;
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "workflow item %u [%p] is activated", wfid, this);
#endif
    }
    void deActivate()
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "workflow item %u [%p] is deActivated", wfid, this);
#endif
        active = false;
    }
    bool isActive() const { return active; }
    void setMode(WFMode _mode)
    {
        mode = _mode;
    }
    void setFailureWfid(unsigned _failure)
    {
        failure = _failure;
    }
    void setSuccessWfid(unsigned _success)
    {
        success = _success;
    }
    void setPersistWfid(unsigned _persistWfid)
    {
        persistWfid = _persistWfid;
    }
    void setPersistCounterpart(CCloneWorkflowItem * _persistCounterpart)
    {
        persistCounterpart = _persistCounterpart;
    }
    CCloneWorkflowItem * queryPersistCounterpart() const { return persistCounterpart; }
    void setPersistLock(IRemoteConnection * thisLock) { persistLock.setown(thisLock); }
    IRemoteConnection * queryPersistLock() const { return persistLock.get(); }
    void setPersistVersion(PersistVersion * _thisPersist) { thisPersist.setown(_thisPersist); }
    PersistVersion *queryPersistVersion() { return thisPersist.get(); }

    void setException(WorkflowException const * e)
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "workflow item %u [%p] has its exception set", wfid, this);
#endif
        WorkflowException * ex = const_cast<WorkflowException *>(e);
        thisException.set(ex);
    }
    void setAbortFlag(bool b)
    {
        executeOnAbort = b;
    }
    void setContingencyWithin(unsigned n)
    {
        withinContingency = n;
    }

    void copy(IConstWorkflowItem const * other)
    {
        wfid = other->queryWfid();
        if(other->isScheduled())
            schedule.setown(new CCloneSchedule(other));
        Owned<IWorkflowDependencyIterator> iter = other->getDependencies();
        for(iter->first(); iter->isValid(); iter->next())
            dependencies.append(iter->query());
        type = other->queryType();
        mode = other->queryMode();
        success = other->querySuccess();
        failure = other->queryFailure();
        recovery = other->queryRecovery();
        retriesAllowed = other->queryRetriesAllowed();
        contingencyFor = other->queryContingencyFor();
        state = other->queryState();
        retriesRemaining = other->queryRetriesRemaining();
        if(state == WFStateFail)
        {
            failcode = other->queryFailCode();
            failmsg.set(other->queryFailMessage());
        }
        eventName.set(other->queryEventName());
        eventExtra.set(other->queryEventExtra());
        other->getPersistName(persistName);
        persistWfid = other->queryPersistWfid();
        scheduledWfid = other->queryScheduledWfid();
        persistCopies = other->queryPersistCopies();
        persistRefresh = other->queryPersistRefresh();
        other->getCriticalName(criticalName);
        other->queryCluster(clusterName);
        other->getLabel(label);
    }
    //info set at compile time
    virtual WorkflowException * queryException() const
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "workflow item %u [%p] has its exception queried", wfid, this);
#endif
         return thisException.get();
    }
    virtual bool         queryAbortFlag() const { return executeOnAbort; }
    virtual unsigned     queryContingencyWithin() const { return withinContingency; }
    virtual unsigned     queryWfid() const { return wfid; }
    virtual bool         isScheduled() const { return schedule.get() != 0; }
    virtual bool         isScheduledNow() const { return schedule && schedule->isNow(); }
    virtual IWorkflowEvent * getScheduleEvent() const { if(schedule) return schedule->getEvent(); else return NULL; }
    virtual unsigned     querySchedulePriority() const { return schedule ? schedule->queryPriority() : 0; }
    virtual bool         hasScheduleCount() const { return schedule ? schedule->hasCount() : false; }
    virtual unsigned     queryScheduleCount() const { return schedule ? schedule->queryCount() : 0; }
    virtual IWorkflowDependencyIterator * getDependencies() const { return new CCloneIterator(dependencies); }
    virtual WFType       queryType() const { return type; }
    virtual WFMode       queryMode() const { return mode; }
    virtual IStringVal & getLabel(IStringVal & val) const { val.set(label.str()); return val; }
    virtual unsigned     querySuccess() const { return success; }
    virtual unsigned     queryFailure() const { return failure; }
    virtual unsigned     queryRecovery() const { return recovery; }
    virtual unsigned     queryRetriesAllowed() const { return retriesAllowed; }
    virtual unsigned     queryContingencyFor() const { return contingencyFor; }
    virtual IStringVal & getPersistName(IStringVal & val) const { val.set(persistName.str()); return val; }
    virtual unsigned     queryPersistWfid() const { return persistWfid; }
    virtual int          queryPersistCopies() const { return persistCopies; }
    virtual bool         queryPersistRefresh() const { return persistRefresh; }
    virtual IStringVal & getCriticalName(IStringVal & val) const { val.set(criticalName.str()); return val; }
    virtual IStringVal & queryCluster(IStringVal & val) const { val.set(clusterName.str()); return val; }
    //info set at run time
    virtual unsigned     queryScheduleCountRemaining() const { return schedule ? schedule->queryCountRemaining() : 0; }
    virtual WFState      queryState() const { return state; }
    virtual unsigned     queryRetriesRemaining() const { return retriesRemaining; }
    virtual int          queryFailCode() const { return failcode; }
    virtual char const * queryFailMessage() const { return failmsg.get(); }
    virtual char const * queryEventName() const { return eventName; }
    virtual char const * queryEventExtra() const { return eventExtra; }
    virtual unsigned     queryScheduledWfid() const { return scheduledWfid; }
    virtual void         setState(WFState _state) { state = _state; }
    virtual bool         testAndDecRetries()
    {
        if(retriesRemaining == 0)
            return false;
        retriesRemaining--;
        return true;
    }
    virtual bool         decAndTestScheduleCountRemaining()
    {
        if(!schedule)
            return true;
        return schedule->decAndTestCountRemaining();
    }
    virtual void incScheduleCount()
    {
        if(schedule)
            schedule->incCountRemaining();
    }
    virtual void         setFailInfo(int code, char const * message)
    {
        failcode = code;
        failmsg.set(message);
    }
    virtual void         setEvent(const char * name, const char * extra)
    {
        eventName.set(name);
        eventExtra.set(extra);
    }
    virtual void         reset()
    {
        retriesRemaining = retriesAllowed;
        if(schedule) schedule->resetCount();
        if(isScheduled())
        {
            if(isScheduledNow())
                setState(WFStateReqd);
            else if (hasScheduleCount() && (queryScheduleCountRemaining() == 0))
                setState(WFStateDone);
            else
                setState(WFStateWait);
        }
        else if(queryType() == WFTypeRecovery)
            setState(WFStateSkip);
        else
            setState(WFStateNull);
    }
};

class CWorkflowItemIterator : public CInterface, implements IWorkflowItemIterator
{
public:
    CWorkflowItemIterator(IPropertyTree * tree) { iter.setown(tree->getElements("Item")); }
    IMPLEMENT_IINTERFACE;
    bool                first() { item.clear(); return iter->first(); }
    bool                isValid() { return iter->isValid(); }
    bool                next() { item.clear(); return iter->next(); }
    IConstWorkflowItem * query() const { if(!item) item.setown(new CWorkflowItem(iter->get())); return item.get(); }
    IWorkflowItem *     get() const { if(!item) item.setown(new CWorkflowItem(iter->get())); return item.getLink(); }
private:
    Owned<IPropertyTreeIterator> iter;
    mutable Owned<CWorkflowItem> item;
};

class CCloneWorkflowItemArray : public CInterface, implements IWorkflowItemArray
{
private:
    class ListItem
    {
    public:
        ListItem(ListItem * _next, IRuntimeWorkflowItem * _item) : next(_next), item(_item) {}
        ListItem * next;
        IRuntimeWorkflowItem * item;
    };

    class ListItemPtr : public CInterface, implements IRuntimeWorkflowItemIterator
    {
    public:
        ListItemPtr(ListItem * _start) : start(_start) { ptr = NULL; }
        IMPLEMENT_IINTERFACE;
        virtual bool         first() { ptr = start; return isValid(); }
        virtual bool         isValid() { return ptr != NULL; }
        virtual bool         next() { ptr = ptr->next; return isValid(); }
        virtual IConstWorkflowItem * query() const { return ptr->item; }
        virtual IRuntimeWorkflowItem * get() const { return LINK(ptr->item); }
    private:
        ListItem * start;
        ListItem * ptr;
    };

    void insert(CCloneWorkflowItem * item)
    {
        if(!item->isScheduled())
            return;
        if(!head)
            head = tail = new ListItem(NULL, item);
        else if(item->querySchedulePriority() > head->item->querySchedulePriority())
            head = new ListItem(head, item);
        else if(item->querySchedulePriority() <= tail->item->querySchedulePriority())
        {
            tail->next = new ListItem(NULL, item);
            tail = tail->next;
        }
        else
        {
            ListItem * finger = head;
            while(item->querySchedulePriority() <= finger->next->item->querySchedulePriority())
                finger = finger->next;
            finger->next = new ListItem(finger->next, item);
        }
    }

public:
    CCloneWorkflowItemArray(unsigned _capacity) : capacity(_capacity), head(NULL), tail(NULL) 
    {
        array = _capacity ? new CCloneWorkflowItem[_capacity] : NULL;
    }
    ~CCloneWorkflowItemArray()
    {
        ListItem * finger = head;
        while(finger)
        {
            ListItem * del = finger;
            finger = finger->next;
            delete del;
        }
        if (array)
            delete [] array;
    }

    IMPLEMENT_IINTERFACE;

    virtual void addClone(IConstWorkflowItem const * other)
    {
        unsigned wfid = other->queryWfid();
        assertex((wfid > 0) && (wfid <= capacity));
        array[wfid-1].copy(other);
        insert(&array[wfid-1]);
    }

    virtual IRuntimeWorkflowItem & queryWfid(unsigned wfid)
    {
        assertex((wfid > 0) && (wfid <= capacity));
        return array[wfid-1];
    }

    virtual unsigned count() const
    {
        return capacity;
    }
    //iterator through the scheduled items (not ALL the items)
    virtual IRuntimeWorkflowItemIterator * getSequenceIterator() { return new ListItemPtr(head); }

    virtual bool hasScheduling() const
    {
        ListItem * finger = head;
        while(finger)
        {
            if(!finger->item->isScheduledNow())
                return true;
            finger = finger->next;
        }
        return false;
    }

private:
    unsigned capacity;
    CCloneWorkflowItem * array;
    ListItem * head;
    ListItem * tail;
};

//-------------------------------------------------------------------------------------------------

WorkflowMachine::WorkflowMachine()
    : ctx(NULL), process(NULL), currentWfid(0), currentScheduledWfid(0), itemsWaiting(0), itemsUnblocked(0), condition(false), logctx(queryDummyContextLogger())
{
}

WorkflowMachine::WorkflowMachine(const IContextLogger &_logctx)
    : ctx(NULL), process(NULL), currentWfid(0), currentScheduledWfid(0), itemsWaiting(0), itemsUnblocked(0), condition(false), logctx(_logctx)
{
}


void WorkflowMachine::addSuccessors()
{
    Owned<IRuntimeWorkflowItemIterator> iter = workflow->getSequenceIterator();
    if (iter->first())
    {
        while (iter->isValid())
        {
            IConstWorkflowItem * item = iter->query();
            if(item->queryState() == WFStateReqd)
            {
                //initial call
                startingWfid = item->queryWfid();
#ifdef TRACE_WORKFLOW
                LOG(MCworkflow, "Item %u has been identified as the 'parent' item, with Reqd state", startingWfid);
#endif
                markDependencies(startingWfid, nullptr, nullptr);
                break;
            }
            if(!iter->next()) break;
        }
    }
#ifdef TRACE_WORKFLOW
    //Outputting debug info about each workflow item.
    unsigned totalDependencies = 0;
    unsigned totalActiveItems = 0;
    unsigned totalInActiveItems = 0;
    unsigned totalDependentSuccessors = 0;
    unsigned totalLogicalSuccessors = 0;
    unsigned totalConditionItems = 0;
    //iterate through the workflow items
    for(int i = 1; i <= workflow->count(); i++)
    {
        CCloneWorkflowItem & cur = queryWorkflowItem(i);
        unsigned numDep = cur.queryNumDependencies();
        unsigned numDepSuc = cur.queryNumDependentSuccessors();
        unsigned numLogSuc = cur.queryNumLogicalSuccessors();
        if(cur.isActive())
            totalActiveItems ++;
        else
            totalInActiveItems ++;
        totalDependencies += numDep;
        totalDependentSuccessors += numDepSuc;
        totalLogicalSuccessors += numLogSuc;
        LOG(MCworkflow, "Item %u has %u dependencies, %u dependent successors and %u logical successors", cur.queryWfid(), numDep, numDepSuc, numLogSuc);

        if(cur.queryMode() == WFModeCondition)
        {
            totalConditionItems++;
        }
    }
    //iterate throught the IntermediaryWorkflow items
    for(int i = 0;  i < logicalWorkflow.size() ; i++)
    {

        IRuntimeWorkflowItem  *tmp = logicalWorkflow[i].get();
        CCloneWorkflowItem * cur = static_cast<CCloneWorkflowItem*>(tmp);
        unsigned numDep = cur->queryNumDependencies();
        unsigned numDepSuc = cur->queryNumDependentSuccessors();
        unsigned numLogSuc = cur->queryNumLogicalSuccessors();
        if(cur->isActive())
            totalActiveItems ++;
        else
            totalInActiveItems ++;
        totalDependencies += numDep;
        totalDependentSuccessors += numDepSuc;
        totalLogicalSuccessors += numLogSuc;
        LOG(MCworkflow, "Runtime item %u has %u dependencies, %u dependent successors and %u logical successors", cur->queryWfid(), numDep, numDepSuc, numLogSuc);
        if(cur->queryMode() == WFModeCondition)
        {
            totalConditionItems++;
        }
    }
    LOG(MCworkflow, "Total dependencies is: %u, total dependent successors is: %u, total logical successors is: %u", totalDependencies, totalDependentSuccessors, totalLogicalSuccessors);
    LOG(MCworkflow, "Total condition items is: %u, total active items is: %u, total inactive items is %u", totalConditionItems, totalActiveItems, totalInActiveItems);
    if(totalDependencies == totalDependentSuccessors)
        LOG(MCworkflow, "dependency and dependent successor count is consistent");
    else
        LOG(MCworkflow, "dependency and dependent successor count is inconsistent");
#endif
}
CCloneWorkflowItem * WorkflowMachine::insertLogicalPredecessor(unsigned successorWfid)
{
    unsigned wfid = workflow->count() + logicalWorkflow.size()+1;
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "new predecessor workflow item %u has been created", wfid);
#endif

    CCloneWorkflowItem * predecessor = new CCloneWorkflowItem(wfid); //initialise the intermediary
    Owned<IRuntimeWorkflowItem> tmp = predecessor;
    logicalWorkflow.push_back(tmp); //adding it to the workflow array

    markDependencies(successorWfid, predecessor, nullptr);
    return predecessor;
}
//logicalPredecessor is for Sequential/Condition/Contingency
void WorkflowMachine::markDependencies(unsigned int wfid, CCloneWorkflowItem *logicalPredecessor, CCloneWorkflowItem * prevOrdered)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Called mark dependents on item %u", wfid);
#endif

    CCloneWorkflowItem & item = queryWorkflowItem(wfid);
    //copy a pointer to the logical predecessor argument
    CCloneWorkflowItem * thisLogicalPredecessor = logicalPredecessor;

    //Ordered causes the effect of logicalPredecessor to skip a generation
    bool alreadyProcessed = !item.isDependentSuccessorsEmpty();
    if(!alreadyProcessed)
    {
        if(item.queryMode() == WFModePersist)
        {
            CCloneWorkflowItem & persistActivator = queryWorkflowItem(item.queryPersistWfid());
            persistActivator.setMode(WFModePersistActivator);
            //NOTE: this means that whenever item would become a logical successor, predecessor is the logical successor as well, since the items come as a pair
            item.setPersistCounterpart(&persistActivator);
            persistActivator.setPersistCounterpart(&item); //the relationship is reciprocal
            persistActivator.setPersistWfid(wfid);
            //the persist Activator must be done before the persist
            persistActivator.addDependentSuccessor(&item);
            logicalPredecessor = &persistActivator;
        }
    }
    if(prevOrdered)
    {
        //this successorship should be added even if cur has already marked dependents
        item.addLogicalSuccessor(prevOrdered);
    }
    if(!prevOrdered)
    {
        if(thisLogicalPredecessor)
        {
            if(!alreadyProcessed)
                thisLogicalPredecessor->addLogicalSuccessor(&item);
        }
        else
        {
            //if logicalPredecessor is nullptr, then the item should be marked as active, since it doesn't have to follow on from anything else to execute.
            if(!item.isActive())
            {
                item.activate();
                //This checks that the current item has already performed the markDependencies algorithm
                if(alreadyProcessed)
                {
                    activateDependencies(item);
                }
            }
        }
    }
    if(alreadyProcessed)
        return;

    Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
    //For Non-Condition items
    if(item.queryMode() != WFModeCondition)
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Item %u is a non-condition item", wfid);
#endif

        CCloneWorkflowItem * prev = nullptr;
        for(iter->first(); iter->isValid(); iter->next())
        {
            CCloneWorkflowItem & cur = queryWorkflowItem(iter->query());
            //prev is the logical predecessor to cur.
            switch(item.queryMode())
            {
            case WFModeOrdered:
                //Note: Ordered doesn't change logicalPredecessor, so that the dependencies of the cur item can use logicalPredecessor. It skips a generation.
                markDependencies(cur.queryWfid(), logicalPredecessor, prev);
                break;
            case WFModeSequential:
                //Note: Sequential changes logicalPredecessor, so that the dependencies of the cur item also depend on prev.
                if(prev)
                    logicalPredecessor = prev;
                //fall through
            default:
                markDependencies(cur.queryWfid(), logicalPredecessor, nullptr);
                break;
            }
            //This happens after markDependencies as a way of ensuring that each item only marks its dependencies once.
            cur.addDependentSuccessor(&item);
            prev = &cur;
        }
    }
    else
    {
        //For Condition items
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Item %u is a condition item", wfid);
#endif
        if(!iter->first())
            throwUnexpected();
        CCloneWorkflowItem & conditionExpression = queryWorkflowItem(iter->query());
        if(!iter->next())
            throwUnexpected();
        unsigned wfidTrue = iter->query();
        unsigned wfidFalse = 0;
        if(iter->next())
            wfidFalse = iter->query();

        conditionExpression.setMode(WFModeConditionExpression);
        markDependencies(conditionExpression.queryWfid(), logicalPredecessor, nullptr);

        //add logical successors
         conditionExpression.addLogicalSuccessor(insertLogicalPredecessor(wfidTrue));
        //add dependent successors
        CCloneWorkflowItem & trueSuccessor = queryWorkflowItem(wfidTrue);
        trueSuccessor.addDependentSuccessor(&item);

        if(wfidFalse)
        {
            //add logical successors
            conditionExpression.addLogicalSuccessor(insertLogicalPredecessor(wfidFalse));
            //add dependent successors
            CCloneWorkflowItem & falseSuccessor = queryWorkflowItem(wfidFalse);
            falseSuccessor.addDependentSuccessor(&item);
            //Decrement this.numDependencies by one, to account for one path not being completed in the future.
            item.atomicDecNumDependencies();
        }
        conditionExpression.addDependentSuccessor(&item);
    }
    //For contingencies, it creates an intermediary item, and inserts it between this item and the contingency
    unsigned successWfid = item.querySuccess();
    if(successWfid)
    {
        item.setSuccessWfid(insertLogicalPredecessor(successWfid)->queryWfid());
    }
    unsigned failureWfid = item.queryFailure();
    if(failureWfid)
    {
        item.setFailureWfid(insertLogicalPredecessor(failureWfid)->queryWfid());
    }
}
void WorkflowMachine::activateDependencies(CCloneWorkflowItem & item)
{
    if(!(item.queryMode() == WFModePersist))
    {
        Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
        for(iter->first(); iter->isValid(); iter->next())
        {
            CCloneWorkflowItem & cur = queryWorkflowItem(iter->query());
            if(!cur.isActive())
            {
                cur.activate();
                activateDependencies(cur);
            }
            //This break is for when a condition item is being activated.
            //Out of its dependencies, only the condition expression should be made active.
            //trueresult and falseresult should be left inactive
            if(item.queryMode() == WFModeCondition)
                break;
        }
    }
}
void WorkflowMachine::addToItemQueue(unsigned wfid)
{
    {
        CriticalBlock thisBlock(queueCritSec);
        wfItemQueue.push(wfid);
    }
    wfItemQueueSem.signal(1);
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "item %u has been added to the item queue", wfid);
#endif
}
void WorkflowMachine::processSuccessors(CCloneWorkflowItem & item, Owned<IWorkflowDependencyIterator> iter, bool isLogical)
{
    //item will never be executed later, if it has been passed as an argument to this function
    if(item.queryWfid() == startingWfid)
    {
        //update stop conditions
        parentReached = true;
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Reached parent");
#endif
        //evaluate stop conditions.
        //If failureClauseOld is false and the workflow has failed, then it needs to check whether there are any pending contingencies
        checkIfDone();
        return;
    }
    //net branches is used for when an item has failed
    unsigned netBranches = 0U;
    //if(abort == item.queryAbortFlag()) MORE: optional check that might increase speed, but may introduce race condition
    for(iter->first();iter->isValid(); iter->next())
    {
        unsigned thisWfid = iter->query();
        CCloneWorkflowItem & cur = queryWorkflowItem(thisWfid);
        //item has an exception, implies that a dependency of cur has failed
        if(item.queryException())
        {
            if(cur.queryException())
            {
                //only process the exception if cur is active
                if(cur.isActive())
                {
                     cur.setAbortFlag(item.queryAbortFlag());
                    addToItemQueue(thisWfid);
                    netBranches++;
                    if(reportFailureToFirstDependant)
                        return;
                }
            }
        }
        //processDependentSuccessors made the current function call
        else if(!isLogical)
        {
            unsigned numPred = cur.atomicDecNumDependencies();
            if((numPred == 1) && cur.isActive())
            {
                cur.setAbortFlag(item.queryAbortFlag());
                //ThreadSafe
                addToItemQueue(thisWfid);
            }
        }
        //processLogicalSuccessors made the current function call
        else
        {
            if(!item.queryContingencyWithin())
            {
                //This means that if the item is already being executed and fails, it will cause a global abort, not a contingency failure
                cur.setContingencyWithin(0);
            }
            //entering activation critical section
            {
                CriticalBlock thisBlock(activationCritSec);
                if(!cur.isActive())
                {
                    cur.activate();
                }
            }
            if(cur.isActive())
            {
                cur.setContingencyWithin(item.queryContingencyWithin());//More: why is this here?
                if(cur.queryNumDependencies() == 0)
                {
                    cur.setAbortFlag(item.queryAbortFlag());
                    //ThreadSafe
                    addToItemQueue(thisWfid);
                }
            }
        }
    }
    if(item.queryException())
        branchCount.fetch_add(netBranches-1);
}
void WorkflowMachine::processDependentSuccessors(CCloneWorkflowItem &item)
{
    processSuccessors(item, item.getDependentSuccessors(), false);
}
void WorkflowMachine::processLogicalSuccessors(CCloneWorkflowItem &item)
{
    processSuccessors(item, item.getLogicalSuccessors(), true);
}
void WorkflowMachine::failDependentSuccessors(CCloneWorkflowItem &item)
{
    //DependentSuccessors never execute before their dependenciess
    WorkflowException * e = item.queryException();
    if(!e)
        return;
    Owned<IWorkflowDependencyIterator> iter = item.getDependentSuccessors();
    for(iter->first();iter->isValid(); iter->next())
    {
        unsigned thisWfid = iter->query();
        CCloneWorkflowItem & cur = queryWorkflowItem(thisWfid);
        if(!item.queryContingencyWithin() ||(item.queryContingencyWithin() == cur.queryContingencyWithin()))
        {
            CriticalBlock thisBlock(exceptionCritSec);
            if(!cur.queryException())
            {
                cur.setException(e);
            }
        }
    }
}
bool WorkflowMachine::activateFailureContingency(CCloneWorkflowItem & item, bool isAborting)
{
    unsigned failureWfid = item.queryFailure();
    if(failureWfid)
    {
        startContingency();
        CCloneWorkflowItem & failureActivator = queryWorkflowItem(failureWfid);
        if(isAborting)
            failureActivator.setAbortFlag(true);
        failureActivator.setContingencyWithin(item.queryWfid());
        processLogicalSuccessors(failureActivator);
        return true;
    }
    return false;
}
void WorkflowMachine::startContingency()
{
    activeContingencies ++;
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Starting a new contingency");
#endif
}
void WorkflowMachine::endContingency()
{
    activeContingencies --;
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Ending a contingency");
#endif
}
void WorkflowMachine::executeItemParallel(unsigned wfid)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Beginning workflow item %u", wfid);
#endif

    CCloneWorkflowItem & item = queryWorkflowItem(wfid);
    //get lock on abort
    {
        CriticalBlock thisBlock(abortCritSec);
        if(abort != item.queryAbortFlag())
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "Ignoring workflow item %u due to abort", wfid);
#endif
            item.deActivate();
            return;
        }
    }
    switch(item.queryState())
    {
    case WFStateDone:
        if (item.queryMode() == WFModePersist)
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "Recheck persist %u", wfid);
#endif
            break;
        }
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Nothing to be done for workflow item %u", wfid);
#endif
        return;
    case WFStateSkip:
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Nothing to be done for workflow item %u", wfid);
#endif
        return;
    case WFStateWait:
        throw new WorkflowException(0, "INTERNAL ERROR: attempting to execute workflow item in wait state", wfid, WorkflowException::SYSTEM, MSGAUD_user);
    case WFStateBlocked:
        throw new WorkflowException(0, "INTERNAL ERROR: attempting to execute workflow item in blocked state", wfid, WorkflowException::SYSTEM, MSGAUD_user);
    case WFStateFail:
        item.reset();
        break;
    }
    if(item.queryException())
    {
        failDependentSuccessors(item);
        bool hasContingency = activateFailureContingency(item, item.queryAbortFlag());
        if(hasContingency)
            return;
        if(item.queryContingencyFor())
        {
            bool success = false;
            switch(item.queryType())
            {
            case WFTypeSuccess:
                success = true;
                //fall through
            case WFTypeFailure:
                //This item must be the last item in the contingency to execute
                endContingency();
                if(checkIfDone())
                    return;
                //A contingency cannot have its own contingency
                processDependentSuccessors(queryWorkflowItem(item.queryContingencyFor()));
                if(success)
                    processLogicalSuccessors(queryWorkflowItem(item.queryContingencyFor()));
                return;
            }
        }
        //else, continue to process logical/dependent successors (further down)
    }
    else if(!item.isActive())
    {
        //should never happen
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Ignoring workflow item %u due to inactive state", wfid);
#endif
        throwUnexpected();
    }
    else
    {
        try
        {
            switch(item.queryMode())
            {
            case WFModeNormal:
            case WFModeOnce:
                doExecuteItemParallel(item);
                break;
            case WFModeCondition:
            case WFModeSequential:
            case WFModeParallel:
                break;
            case WFModeConditionExpression:
                doExecuteConditionExpression(item);
                break;
            case WFModePersist:
                doExecutePersistItemParallel(item);
                break;
            case WFModePersistActivator:
                doExecutePersistActivatorParallel(item);
                return;
            case WFModeCritical:
                //mightn't work
                doExecuteCriticalItem(item);
                break;
            case WFModeBeginWait:
            case WFModeWait:
                throwUnexpected();
                break;
            default:
                throwUnexpected();
            }
            bool success = false;
            switch(item.queryType())
            {
            case WFTypeNormal:
                if(item.isScheduled() && !item.isScheduledNow() && item.decAndTestScheduleCountRemaining())
                    throwUnexpected();
                else
                    item.setState(WFStateDone);
                break;
            case WFTypeSuccess:
                success = true;
                //fall through
            case WFTypeFailure:
                item.setState(WFStateNull);
                //This item must be the last item in the contingency to execute
                endContingency();
                if(checkIfDone())
                    return;
                //A contingency cannot have its own contingency
                processDependentSuccessors(queryWorkflowItem(item.queryContingencyFor()));
                if(success)
                    processLogicalSuccessors(queryWorkflowItem(item.queryContingencyFor()));
                return;
            case WFTypeRecovery:
                item.setState(WFStateSkip);
                break;
            }
            unsigned successWfid = item.querySuccess();
            if(successWfid)
            {
                startContingency();
                CCloneWorkflowItem & successActivator = queryWorkflowItem(successWfid);
                successActivator.setAbortFlag(item.queryAbortFlag());
                successActivator.setContingencyWithin(item.queryWfid());
                processLogicalSuccessors(successActivator);
                return;
            }
        }
        catch(WorkflowException const * e)
        {
            Owned<const WorkflowException> savedException = e;
            bool hasContingency = handleFailureParallel(item, e, false);
            //If the contingency exists, it must be fully performed before processSuccessors is called on the current item
            //Until the clause finishes, any items dependent on the current item shouldn't execute.
            if(hasContingency)
                return;
        }
    }
    bool inDate;
    // NOTE - it is unlikely that this comparison will become out of date, but it is accounted for, above, with: if(abort != item.queryAbortFlag())
    {
        CriticalBlock thisBlock(abortCritSec);
        inDate = (abort == item.queryAbortFlag());
    }
    if(inDate && !done)
    {
        processLogicalSuccessors(item);
        processDependentSuccessors(item);
    }
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Done workflow item %u", wfid);
#endif
}

void WorkflowMachine::doExecuteItemParallel(IRuntimeWorkflowItem & item)
{
    try
    {
        performItemParallel(item.queryWfid());
    }
    catch(WorkflowException * ein)
    {
        if (ein->queryWfid() == 0)
        {
            StringBuffer msg;
            ein->errorMessage(msg);
            WorkflowException * newException = new WorkflowException(ein->errorCode(), msg.str(), item.queryWfid(), ein->queryType(), ein->errorAudience());
            ein->Release();
            ein = newException;
        }

        if(ein->queryType() == WorkflowException::ABORT)
            throw ein;

        //if(!attemptRetry(item, 0, scheduledWfid))
        {
            throw ein;
        }
        ein->Release();
    }
    catch(IException * ein)
    {
        checkForAbort(item.queryWfid(), ein);
        //if(!attemptRetry(item, 0, scheduledWfid))
        {
            StringBuffer msg;
            ein->errorMessage(msg);
            WorkflowException::Type type = ((dynamic_cast<IUserException *>(ein) != NULL) ? WorkflowException::USER : WorkflowException::SYSTEM);
            WorkflowException * eout = new WorkflowException(ein->errorCode(), msg.str(), item.queryWfid(), type, ein->errorAudience());
            ein->Release();
            throw eout;
        }
        ein->Release();
    }
}

void WorkflowMachine::doExecuteConditionExpression(CCloneWorkflowItem & item)
{
    bool result;
    {
        //To prevent "condition" from causing a race condition
        CriticalBlock thisBlock(conditionCritSec);
        doExecuteItemParallel(item);
        result = condition;
    }
    //index 0 contains true successor, index 1 contains false successor
    Owned<IWorkflowDependencyIterator> iter = item.getLogicalSuccessors();
    if(!iter->first())
        throwUnexpected();
    unsigned wfidTrue = iter->query();
    unsigned wfidFalse = 0;
    if(iter->next())
        wfidFalse = iter->query();
    if(result)
    {
        processLogicalSuccessors(queryWorkflowItem(wfidTrue));
    }
    else
    {
        if(wfidFalse)
            processLogicalSuccessors(queryWorkflowItem(wfidFalse));
        else
            processDependentSuccessors(item); //this will happen twice overall
    }
    item.removeLogicalSuccessors();
}
void WorkflowMachine::doExecutePersistActivatorParallel(CCloneWorkflowItem & item)
{
    unsigned wfid = item.queryPersistWfid();
    CCloneWorkflowItem * persistItem = item.queryPersistCounterpart();
    SCMStringBuffer name;
    const char *logicalName = persistItem->getPersistName(name).str();
    Owned<IRemoteConnection> persistLock;
    persistLock.setown(startPersist(logicalName));
    doExecuteItemParallel(item);  // generated code should end up calling back to returnPersistVersion, which sets persist
    Owned<PersistVersion> thisPersist;
    thisPersist.setown(getClearPersistVersion(wfid, item.queryWfid()));
    if (strcmp(logicalName, thisPersist->logicalName.get()) != 0)
    {
        StringBuffer errmsg;
        errmsg.append("Failed workflow/persist consistency check: wfid ").append(wfid).append(", WU persist name ").append(logicalName).append(", runtime persist name ").append(thisPersist->logicalName.get());
        throw MakeStringExceptionDirect(0, errmsg.str());
    }
    if (!checkFreezePersists(logicalName, thisPersist->eclCRC))
    {
        if(!isPersistUptoDate(persistLock, *persistItem, logicalName, thisPersist->eclCRC, thisPersist->allCRC, thisPersist->isFile))
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "Persist is not up to date. (item %u)", wfid);
#endif
            //save thisPersist in activator item
            item.setPersistVersion(thisPersist.getClear());
            //save persist lock in persist item
            persistItem->setPersistLock(persistLock.getClear());
            processLogicalSuccessors(item);
            processDependentSuccessors(item);
            return;
        }
    }
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Persist is up to date. (item %u)", wfid);
#endif
    logctx.CTXLOG("Finished persists - add to read lock list");
    persistItem->setPersistLock(persistLock.getClear());
    processDependentSuccessors(*persistItem);
    processLogicalSuccessors(*persistItem);
}
void WorkflowMachine::doExecutePersistItemParallel(CCloneWorkflowItem & item)
{
    SCMStringBuffer name;
    const char *logicalName = item.getPersistName(name).str();
    int maxPersistCopies = item.queryPersistCopies();

    IRemoteConnection * persistLock = item.queryPersistLock();
    PersistVersion * thisPersist=  item.queryPersistCounterpart()->queryPersistVersion();

    readyPersistStore(logicalName, maxPersistCopies);

    doExecuteItemParallel(item);
    updatePersist(persistLock, logicalName, thisPersist->eclCRC, thisPersist->allCRC);

    logctx.CTXLOG("Finished persists - add to read lock list");
    //persist lock is saved in the item
}
void WorkflowMachine::performItemParallel(unsigned wfid)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Performing workflow item %u", wfid);
#endif
    timestamp_type startTime = getTimeStampNowValue();
    CCycleTimer timer;
    process->perform(ctx, wfid);
    noteTiming(wfid, startTime, timer.elapsedNs());
}
bool WorkflowMachine::handleFailureParallel(CCloneWorkflowItem & item, WorkflowException const * e, bool isDep)
{
    item.setException(e);
    failDependentSuccessors(item);
    StringBuffer msg;
    e->errorMessage(msg).append(" (in item ").append(e->queryWfid()).append(")");
    if(isDep)
        logctx.logOperatorException(NULL, NULL, 0, "Dependency failure for workflow item %u: %d: %s", item.queryWfid(), e->errorCode(), msg.str());
    else
        logctx.logOperatorException(NULL, NULL, 0, "%d: %s", e->errorCode(), msg.str());
    item.setFailInfo(e->errorCode(), msg.str());
    switch(item.queryType())
    {
    case WFTypeNormal:
        item.setState(WFStateFail);
        break;
    case WFTypeSuccess:
    case WFTypeFailure:
        item.setState(WFStateNull);
        break;
    case WFTypeRecovery:
        item.setState(WFStateSkip);
        break;
    }
    if(!item.queryContingencyWithin())
    {
        WorkflowException * ex = const_cast<WorkflowException *>(e);
        runtimeError.set(ex);
        branchCount ++;
        //get lock on abort
        {
            CriticalBlock thisBlock(abortCritSec);
            abort = true;
            item.setAbortFlag(true);
            return activateFailureContingency(item, true);
        }
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Workflow item %u failed. Aborting task", item.queryWfid());
#endif
    }
    else
    {
        WFState contingencyState = queryWorkflowItem(item.queryContingencyWithin()).queryState();
        if(contingencyState == WFStateDone)
            reportContingencyFailure("Success", const_cast<WorkflowException *>(e));
        else if(contingencyState == WFStateFail)
             reportContingencyFailure("Failure", const_cast<WorkflowException *>(e));
        else
             reportContingencyFailure("Unknown", const_cast<WorkflowException *>(e));
        return activateFailureContingency(item, item.queryAbortFlag());
    }
}
CCloneWorkflowItem &WorkflowMachine::queryWorkflowItem(unsigned wfid)
{
    if(wfid <= workflow->count())
    {
        return static_cast<CCloneWorkflowItem&>(workflow->queryWfid(wfid));
    }
    else
    {
        unsigned index = wfid - workflow->count() - 1;
        if(index >= logicalWorkflow.size())
            throwUnexpected();
        return static_cast<CCloneWorkflowItem&>(*logicalWorkflow[index].get());
    }
}
void WorkflowMachine::initialiseItemQueue()
{
    //Items without any dependencies are ready be executed (if they are active)
    //both regular and logical workflow items should start on the queue

    //regular workflow items
    for(int i = 1; i <= workflow->count(); i++)
    {
        CCloneWorkflowItem & cur = queryWorkflowItem(i);

        if((cur.queryNumDependencies() == 0) && cur.isActive())
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "item %u has been added to the inital task queue", cur.queryWfid());
#endif
            wfItemQueue.push(i);
        }
    }
    //logical workflow items (currently, only persist items may start as active)
    for(int i = 0;  i < logicalWorkflow.size() ; i++)
    {
        IRuntimeWorkflowItem  *tmp = logicalWorkflow[i].get();
        CCloneWorkflowItem * cur = static_cast<CCloneWorkflowItem*>(tmp);
        if((cur->queryNumDependencies() == 0) && cur->isActive())
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "item %u has been added to the inital task queue", cur->queryWfid());
#endif
            wfItemQueue.push(cur->queryWfid());
        }
    }
    wfItemQueueSem.signal(wfItemQueue.size());
}
bool WorkflowMachine::checkIfDone()
{
    if((activeContingencies == 0) && (parentReached))
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "WorkflowMachine::checkifDone. Final check. Branch count: %u", branchCount.load());
#endif
        if((branchCount == 0) || reportFailureToFirstDependant)
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "workflow done");
#endif
            done = true;
            wfItemQueueSem.signal(numThreads);
            return true;
        }
    }
    return false;
}
void WorkflowMachine::processWfItems()
{
    while(!done)
    {
        wfItemQueueSem.wait();
        if(!done)
        {
            unsigned currentWfid = 0;
            {
                CriticalBlock thisBlock(queueCritSec);
                currentWfid = wfItemQueue.front();
                wfItemQueue.pop();
            }
            try
            {
                executeItemParallel(currentWfid);
            }
            //terminate threads on fatal exception and save error
            catch(WorkflowException * e)
            {
                runtimeError.setown(e);
                done = true;
                wfItemQueueSem.signal(numThreads); //MORE: think about interrupting other threads
                break;
            }
        }
    }
}
void WorkflowMachine::performParallel(IGlobalCodeContext *_ctx, IEclProcess *_process)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "starting perform parallel");
#endif
    ctx = _ctx;
    process = _process;

    //relink workflow
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Starting to mark Items with their successors");
#endif
    addSuccessors();
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Finished marking Items with their successors");
#endif

    //add initial values
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Adding initial workflow items");
#endif
    initialiseItemQueue();

#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Initialising threads");
#endif

    //initialise thread count
    numThreads = getThreadNumFlag();
    if(numThreads < 1)
        numThreads = 4;
    unsigned maxThreads = getAffinityCpus();
    if(numThreads > maxThreads)
        numThreads = maxThreads;
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "num threads = %u", numThreads);
#endif

    std::vector<std::thread *> threads(numThreads);
    //NOTE: Initial work items have already been added to the queue by initialiseItemQueue() above
    //Start threads
    for(int i=0; i < numThreads; i++)
        threads[i] = new std::thread([this]() {  this->processWfItems(); });

#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Calling join threads");
#endif
    //wait for threads to process the workflow items, and then exit when all the work is done
    for(int i=0; i < numThreads; i++)
        threads[i]->join();

#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Destroying threads");
#endif
    for(int i=0; i < numThreads; i++)
        delete threads[i];

    if(runtimeError)
        throw runtimeError;
}
bool WorkflowMachine::isParallelViable()
{
    //initialise parallel flag from workunit
    parallel = getParallelFlag();
    if(!parallel)
    {
        return false;
    }
    for(int i = 1; i <= workflow->count(); i++)
    {
        CCloneWorkflowItem & cur = queryWorkflowItem(i);
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Checking Item %u to decide if parallel is viable", i);
#endif
        //Blacklist of currently unsupported modes/types
        switch(cur.queryMode())
        {
        case WFModeWait:
        case WFModeBeginWait:
        case WFModeCritical:
        //case WFModePersist:
            return false;
        }
        switch(cur.queryType())
        {
        case WFTypeRecovery:
            return false;
        }
        //switch(cur.queryState())
        if(cur.isScheduled() && (!cur.isScheduledNow()))
            return false;
    }
    return true;
}
//The process parameter defines the c++ task associated with each workflowItem
//These are executed in the context/scope of the 'agent' which calls perform()
void WorkflowMachine::perform(IGlobalCodeContext *_ctx, IEclProcess *_process)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "starting perform");
#endif
    ctx = _ctx;
    process = _process;

    //This is where the 'agent' initialises the workflow engine with an array of workflowItems, with their dependencies
    begin();

    if(isParallelViable())
    {
        performParallel(_ctx, _process);
        return;
    }
    Owned<WorkflowException> error;
    bool scheduling = workflow->hasScheduling();
    if(scheduling)
        schedulingStart();
    bool more = false;
    do
    {
        Owned<IRuntimeWorkflowItem> item;
        Owned<IRuntimeWorkflowItemIterator> iter = workflow->getSequenceIterator();
        itemsWaiting = 0;
        itemsUnblocked = 0;
        if (iter->first())
        {
            while (iter->isValid())
            {
                try
                {
                    item.setown(iter->get());
                    switch(item->queryState())
                    {
                    case WFStateReqd:
                    case WFStateFail:
                        if(!error)
                        {
                            unsigned wfid = item->queryWfid();
                            executeItem(wfid, wfid);
                        }
                        break;
                    }
                }
                catch(WorkflowException * e)
                {
                    error.setown(e);
                }
                if(item->queryState() == WFStateWait) itemsWaiting++;
                if(error) break; //MORE: will not want to break in situations where there might be pending contingency clauses
                if(scheduling && schedulingPull())
                {
                    itemsWaiting = 0;
                    iter.setown(workflow->getSequenceIterator());
                    if(!iter->first()) break;
                }
                else
                    if(!iter->next()) break;
            }
        }
        if(error) break; //MORE: will not want to break in situations where there might be pending contingency clauses
        if(scheduling)
            more = schedulingPullStop();
    } while(more || itemsUnblocked);
    end();
    if(error)
        throw error.getLink();
}

bool WorkflowMachine::executeItem(unsigned wfid, unsigned scheduledWfid)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Beginning workflow item %u", wfid);
#endif
    IRuntimeWorkflowItem & item = workflow->queryWfid(wfid);
    switch(item.queryState())
    {
    case WFStateDone:
        if (item.queryMode() == WFModePersist)
        {
#ifdef TRACE_WORKFLOW
            LOG(MCworkflow, "Recheck persist %u", wfid);
#endif
            break;
        }
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Nothing to be done for workflow item %u", wfid);
#endif
        return true;
    case WFStateSkip:
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Nothing to be done for workflow item %u", wfid);
#endif
        return true;
    case WFStateWait:
        throw new WorkflowException(0, "INTERNAL ERROR: attempting to execute workflow item in wait state", wfid, WorkflowException::SYSTEM, MSGAUD_user);
    case WFStateBlocked:
        throw new WorkflowException(0, "INTERNAL ERROR: attempting to execute workflow item in blocked state", wfid, WorkflowException::SYSTEM, MSGAUD_user);
    case WFStateFail:
        item.reset();
        break;
    }

    switch(item.queryMode())
    {
    case WFModeNormal:
    case WFModeOnce:
        if (!doExecuteItemDependencies(item, wfid))
            return false;
        doExecuteItem(item, scheduledWfid);
        break;
    case WFModeCondition:
        if (!doExecuteConditionItem(item, scheduledWfid))
            return false;
        break;
    case WFModeSequential:
    case WFModeParallel:
        if (!doExecuteItemDependencies(item, scheduledWfid))
            return false;
        break;
    case WFModePersist:
        doExecutePersistItem(item);
        break;
    case WFModeCritical:
        doExecuteCriticalItem(item);
        break;
    case WFModeBeginWait:
        doExecuteBeginWaitItem(item, scheduledWfid);
        item.setState(WFStateDone);
        return false;
    case WFModeWait:
        doExecuteEndWaitItem(item);
        break;
    default:
        throwUnexpected();
    }

    switch(item.queryType())
    {
    case WFTypeNormal:
        if(item.isScheduled() && !item.isScheduledNow() && item.decAndTestScheduleCountRemaining())
            item.setState(WFStateWait);
        else
            item.setState(WFStateDone);
        break;
    case WFTypeSuccess:
    case WFTypeFailure:
        item.setState(WFStateNull);
        break;
    case WFTypeRecovery:
        item.setState(WFStateSkip);
        break;
    }
    if(item.querySuccess())
    {
        try
        {
            executeItem(item.querySuccess(), scheduledWfid);
        }
        catch(WorkflowException * ce)
        {
            if(ce->queryType() == WorkflowException::ABORT)
                throw;
            reportContingencyFailure("SUCCESS", ce);
            ce->Release();
        }
    }
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Done workflow item %u", wfid);
#endif
    return true;
}

bool WorkflowMachine::doExecuteItemDependencies(IRuntimeWorkflowItem & item, unsigned scheduledWfid)
{
    Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
    for(iter->first(); iter->isValid(); iter->next())
    {
        if (!doExecuteItemDependency(item, iter->query(), scheduledWfid, false))
            return false;
    }
    return true;
}

bool WorkflowMachine::doExecuteItemDependency(IRuntimeWorkflowItem & item, unsigned wfid, unsigned scheduledWfid, bool alwaysEvaluate)
{
    try
    {
        if (alwaysEvaluate)
            workflow->queryWfid(wfid).setState(WFStateNull);

        return executeItem(wfid, scheduledWfid);
    }
    catch(WorkflowException * e)
    {
        if(e->queryType() == WorkflowException::ABORT)
            throw;
        if(!attemptRetry(item, wfid, scheduledWfid))
        {
            handleFailure(item, e, true);
            throw;
        }
        e->Release();
    }
    return true;//more!
}

void WorkflowMachine::doExecuteItem(IRuntimeWorkflowItem & item, unsigned scheduledWfid)
{
    try
    {
        performItem(item.queryWfid(), scheduledWfid);
    }
    catch(WorkflowException * ein)
    {
        if (ein->queryWfid() == 0)
        {
            StringBuffer msg;
            ein->errorMessage(msg);
            WorkflowException * newException = new WorkflowException(ein->errorCode(), msg.str(), item.queryWfid(), ein->queryType(), ein->errorAudience());
            ein->Release();
            ein = newException;
        }

        if(ein->queryType() == WorkflowException::ABORT)
            throw ein;

        if(!attemptRetry(item, 0, scheduledWfid))
        {
            handleFailure(item, ein, true);
            throw ein;
        }
        ein->Release();
    }
    catch(IException * ein)
    {
        checkForAbort(item.queryWfid(), ein);
        if(!attemptRetry(item, 0, scheduledWfid))
        {
            StringBuffer msg;
            ein->errorMessage(msg);
            WorkflowException::Type type = ((dynamic_cast<IUserException *>(ein) != NULL) ? WorkflowException::USER : WorkflowException::SYSTEM);
            WorkflowException * eout = new WorkflowException(ein->errorCode(), msg.str(), item.queryWfid(), type, ein->errorAudience());
            ein->Release();
            handleFailure(item, eout, false);
            throw eout;
        }
        ein->Release();
    }
}

bool WorkflowMachine::doExecuteConditionItem(IRuntimeWorkflowItem & item, unsigned scheduledWfid)
{
    Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
    if(!iter->first()) throwUnexpected();
    unsigned wfidCondition = iter->query();
    if(!iter->next()) throwUnexpected();
    unsigned wfidTrue = iter->query();
    unsigned wfidFalse = 0;
    if(iter->next()) wfidFalse = iter->query();
    if(iter->next()) throwUnexpected();

    if (!doExecuteItemDependency(item, wfidCondition, scheduledWfid, true))
        return false;
    if(condition)
        return doExecuteItemDependency(item, wfidTrue, scheduledWfid, false);
    else if (wfidFalse)
        return doExecuteItemDependency(item, wfidFalse, scheduledWfid, false);
    return true;
}

void WorkflowMachine::doExecuteBeginWaitItem(IRuntimeWorkflowItem & item, unsigned scheduledWfid)
{
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Begin wait for workflow item %u sched %u", item.queryWfid(), scheduledWfid);
#endif
    //Block execution of the currently executing scheduled item
    IRuntimeWorkflowItem & scheduledItem = workflow->queryWfid(scheduledWfid);
    assertex(scheduledItem.queryState() == WFStateReqd);
    scheduledItem.setState(WFStateBlocked);

    //And increment the count on the wait wf item so it becomes active
    Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
    if(!iter->first()) throwUnexpected();
    unsigned waitWfid = iter->query();
    if(iter->next()) throwUnexpected();

    IRuntimeWorkflowItem & waitItem = workflow->queryWfid(waitWfid);
    assertex(waitItem.queryState() == WFStateDone);
    waitItem.incScheduleCount();
    waitItem.setState(WFStateWait);
    itemsWaiting++;
}

void WorkflowMachine::doExecuteEndWaitItem(IRuntimeWorkflowItem & item)
{
    //Unblock the scheduled workflow item, which should mean execution continues.
    unsigned scheduledWfid = item.queryScheduledWfid();
#ifdef TRACE_WORKFLOW
    LOG(MCworkflow, "Finished wait for workflow sched %u", scheduledWfid);
#endif
    IRuntimeWorkflowItem & scheduledItem = workflow->queryWfid(scheduledWfid);
    assertex(scheduledItem.queryState() == WFStateBlocked);
    scheduledItem.setState(WFStateReqd);
    itemsUnblocked++;

    //Note this would be more efficient implemented more like a state machine 
    //(with next processing rather than walking from the top down), 
    //but that will require some more work.
}


bool WorkflowMachine::isOlderThanPersist(time_t when, IRuntimeWorkflowItem & item)
{
    time_t thisTime;
    if (!getPersistTime(thisTime, item))
        return false;  // if no time must be older than the persist
    return when < thisTime;
}

bool WorkflowMachine::isOlderThanInputPersists(time_t when, IRuntimeWorkflowItem & item)
{
    Owned<IWorkflowDependencyIterator> iter = item.getDependencies();
    ForEach(*iter)
    {
        unsigned cur = iter->query();

        IRuntimeWorkflowItem & other = workflow->queryWfid(cur);
        if (isPersist(other))
        {
            if (isOlderThanPersist(when, other))
                return true;
        }
        else
        {
            if (isOlderThanInputPersists(when, other))
                return true;
        }
    }
    return false;
}

bool WorkflowMachine::isItemOlderThanInputPersists(IRuntimeWorkflowItem & item)
{
    time_t curWhen;
    if (!getPersistTime(curWhen, item))
        return false; // if no time then old and can't tell

    return isOlderThanInputPersists(curWhen, item);
}

void WorkflowMachine::performItem(unsigned wfid, unsigned scheduledWfid)
{
#ifdef TRACE_WORKFLOW
    if(currentWfid)
        LOG(MCworkflow, "Branching from workflow item %u", currentWfid);
    LOG(MCworkflow, "Performing workflow item %u", wfid);
#endif
    wfidStack.append(currentWfid);
    wfidStack.append(scheduledWfid);
    currentWfid = wfid;
    currentScheduledWfid = scheduledWfid;
    timestamp_type startTime = getTimeStampNowValue();
    CCycleTimer timer;
    process->perform(ctx, wfid);
    noteTiming(wfid, startTime, timer.elapsedNs());
    scheduledWfid = wfidStack.popGet();
    currentWfid = wfidStack.popGet();
    if(currentWfid)
    {
#ifdef TRACE_WORKFLOW
        LOG(MCworkflow, "Returning to workflow item %u", currentWfid);
#endif
    }
}

bool WorkflowMachine::attemptRetry(IRuntimeWorkflowItem & item, unsigned dep, unsigned scheduledWfid)
{
    unsigned wfid = item.queryWfid();
    unsigned recovery = item.queryRecovery();
    if(!recovery)
        return false;
    while(item.testAndDecRetries())
    {
        bool okay = true;
        try
        {
            workflow->queryWfid(recovery).setState(WFStateNull);
            executeItem(recovery, recovery);
            if(dep)
                executeItem(dep, scheduledWfid);
            else
                performItem(wfid, scheduledWfid);
        }
        catch(WorkflowException * ce)
        {
            okay = false;
            if(ce->queryType() == WorkflowException::ABORT)
                throw;
            reportContingencyFailure("RECOVERY", ce);
            ce->Release();
        }
        catch(IException * ce)
        {
            okay = false;
            checkForAbort(wfid, ce);
            reportContingencyFailure("RECOVERY", ce);
            ce->Release();
        }
        if(okay)
            return true;
    }
    return false;
}

void WorkflowMachine::handleFailure(IRuntimeWorkflowItem & item, WorkflowException const * e, bool isDep)
{
    StringBuffer msg;
    e->errorMessage(msg).append(" (in item ").append(e->queryWfid()).append(")");
    if(isDep)
        logctx.logOperatorException(NULL, NULL, 0, "Dependency failure for workflow item %u: %d: %s", item.queryWfid(), e->errorCode(), msg.str());
    else
        logctx.logOperatorException(NULL, NULL, 0, "%d: %s", e->errorCode(), msg.str());
    item.setFailInfo(e->errorCode(), msg.str());
    switch(item.queryType())
    {
    case WFTypeNormal:
        item.setState(WFStateFail);
        break;
    case WFTypeSuccess:
    case WFTypeFailure:
        item.setState(WFStateNull);
        break;
    case WFTypeRecovery:
        item.setState(WFStateSkip);
        break;
    }
    unsigned failureWfid = item.queryFailure();
    if(failureWfid)
    {
        try
        {
            executeItem(failureWfid, failureWfid);
        }
        catch(WorkflowException * ce)
        {
            if(ce->queryType() == WorkflowException::ABORT)
                throw;
            reportContingencyFailure("FAILURE", ce);
            ce->Release();
        }
    }
}

int WorkflowMachine::queryLastFailCode() const
{
    unsigned wfidFor = workflow->queryWfid(currentWfid).queryContingencyFor();
    if(!wfidFor)
        return 0;
    return workflow->queryWfid(wfidFor).queryFailCode();
}

char const * WorkflowMachine::queryLastFailMessage() const
{
    unsigned wfidFor = workflow->queryWfid(currentWfid).queryContingencyFor();
    if(!wfidFor)
        return "";
    char const * ret = workflow->queryWfid(wfidFor).queryFailMessage();
    return ret ? ret : "";
}

const char * WorkflowMachine::queryEventName() const
{
    //MORE: This doesn't work so well once we've done SEQUENTIAL transforms if they split a wf item into 2
    return workflow->queryWfid(currentWfid).queryEventName();
}

const char * WorkflowMachine::queryEventExtra() const
{
    //MORE: This doesn't work so well once we've done SEQUENTIAL transforms if they split a wf item into 2
    return workflow->queryWfid(currentWfid).queryEventExtra();
}


IWorkflowItemIterator *createWorkflowItemIterator(IPropertyTree *p)
{
    return new CWorkflowItemIterator(p);
}

IWorkflowItemArray *createWorkflowItemArray(unsigned size)
{
    return new CCloneWorkflowItemArray(size);
}

IWorkflowItem *createWorkflowItem(IPropertyTree * ptree, unsigned wfid, WFType type, WFMode mode, unsigned success, unsigned failure, unsigned recovery, unsigned retriesAllowed, unsigned contingencyFor)
{
    return new CWorkflowItem(ptree, wfid, type, mode, success, failure, recovery, retriesAllowed, contingencyFor);
}
