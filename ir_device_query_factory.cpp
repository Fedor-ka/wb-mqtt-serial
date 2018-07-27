#include "ir_device_query_factory.h"
#include "ir_device_query.h"
#include "serial_device.h"
#include "memory_block.h"
#include "virtual_register.h"

#include <iostream>
#include <cassert>

using namespace std;

namespace // utility
{
    inline uint32_t GetMaxHoleSize(const PMemoryBlock & first, const PMemoryBlock & last)
    {
        assert(first->Address <= last->Address);

        // perform lookup not on set itself but on range - thus taking into account all created registers
        const auto & memoryBlockSetView = TSerialDevice::StaticCreateMemoryBlockRange(first, last);

        uint32_t hole = 0;

        int prev = -1;
        auto end = memoryBlockSetView.end();
        for (auto itReg = memoryBlockSetView.First; itReg != end; ++itReg) {
            const auto & mb = *itReg;

            assert((int)mb->Address > prev);

            if (prev >= 0) {
                hole = max(hole, (mb->Address - prev) - 1);
            }

            prev = mb->Address;
        }

        return hole;
    }

    inline uint32_t GetMaxHoleSize(const TPSet<PMemoryBlock> & memoryBlockSet)
    {
        return GetMaxHoleSize(*memoryBlockSet.begin(), *memoryBlockSet.rbegin());
    }

    inline uint32_t GetRegCount(const PMemoryBlock & first, const PMemoryBlock & last)
    {
        assert(first->Address <= last->Address);

        return last->Address - first->Address + 1;
    }

    inline uint32_t GetRegCount(const TPSet<PMemoryBlock> & memoryBlockSet)
    {
        return GetRegCount(*memoryBlockSet.begin(), *memoryBlockSet.rbegin());
    }

    inline const TMemoryBlockType & GetType(const TPSet<PMemoryBlock> & memoryBlockSet)
    {
        return (*memoryBlockSet.begin())->Type;
    }

    inline uint16_t GetSize(const TPSet<PMemoryBlock> & memoryBlockSet)
    {
        return (*memoryBlockSet.begin())->Size;
    }

    bool IsReadOperation(EQueryOperation operation)
    {
        switch(operation) {
            case EQueryOperation::Read:
                return true;
            case EQueryOperation::Write:
                return false;
            default:
                assert(false);
                throw TSerialDeviceException("unknown operation: " + to_string((int)operation));
        }
    }

    template <class Query>
    void AddQueryImpl(TAssociatedMemoryBlockSet && memoryBlocks, TPSet<PIRDeviceQuery> & result)
    {
        bool inserted = result.insert(TIRDeviceQueryFactory::CreateQuery<Query>(move(memoryBlocks))).second;
        assert(inserted);
    }

    template <class Query>
    void AddQueryImpl(TAssociatedMemoryBlockSet && memoryBlocks, list<PIRDeviceQuery> & result)
    {
        result.push_back(TIRDeviceQueryFactory::CreateQuery<Query>(move(memoryBlocks)));
    }

    template <class Query>
    void AddQuery(TAssociatedMemoryBlockSet && memoryBlocks, TQueries & result)
    {
        AddQueryImpl<Query>(move(memoryBlocks), result);
    }
}

const TIRDeviceQueryFactory::EQueryGenerationPolicy TIRDeviceQueryFactory::Default = TIRDeviceQueryFactory::Minify;

vector<pair<TIntervalMs, PIRDeviceQuerySet>> TIRDeviceQueryFactory::GenerateQuerySets(const vector<PVirtualRegister> & virtualRegisters, EQueryOperation operation)
{
    vector<pair<TIntervalMs, PIRDeviceQuerySet>> querySetsByPollInterval;
    {
        map<int64_t, vector<PVirtualRegister>> virtualRegistersByTypeAndInterval;
        vector<int64_t> pollIntervals;  // for order preservation

        for (const auto & vreg: virtualRegisters) {
            int64_t pollInterval = vreg->PollInterval.count();

            auto & virtualRegisterSets = virtualRegistersByTypeAndInterval[pollInterval];

            if (virtualRegisterSets.empty()) {
                pollIntervals.push_back(pollInterval);
            }

            virtualRegisterSets.push_back(vreg);
        }

        for (auto & _pollInterval: pollIntervals) {
            auto pollInterval = TIntervalMs(_pollInterval);

            auto it = virtualRegistersByTypeAndInterval.find(_pollInterval);
            assert(it != virtualRegistersByTypeAndInterval.end());

            const auto & registers = it->second;

            const auto & querySet = std::make_shared<TIRDeviceQuerySet>(registers, operation);
            querySetsByPollInterval.push_back({ pollInterval, querySet });
        }
    }

    return querySetsByPollInterval;
}

TQueries TIRDeviceQueryFactory::GenerateQueries(const vector<PVirtualRegister> & virtualRegisters, EQueryOperation operation, EQueryGenerationPolicy policy)
{
    TAssociatedMemoryBlockList groupedMemoryBlocks;
    for (const auto & vreg: virtualRegisters) {
        groupedMemoryBlocks.push_back({ vreg->GetMemoryBlocks(), { vreg } });
    }

    return GenerateQueries(move(groupedMemoryBlocks), operation, policy);
}

TQueries TIRDeviceQueryFactory::GenerateQueries(const vector<PVirtualValue> & virtualValues, EQueryOperation operation, EQueryGenerationPolicy policy)
{
    TAssociatedMemoryBlockList groupedMemoryBlocks;
    for (const auto & virtualValue: virtualValues) {
        auto vreg = dynamic_pointer_cast<TVirtualRegister>(virtualValue);
        assert(vreg);

        groupedMemoryBlocks.push_back({ vreg->GetMemoryBlocks(), { vreg } });
    }

    return GenerateQueries(move(groupedMemoryBlocks), operation, policy);
}

TQueries TIRDeviceQueryFactory::GenerateQueries(
    TAssociatedMemoryBlockList && memoryBlockSets,
    EQueryOperation operation,
    EQueryGenerationPolicy policy)
{
    assert(!memoryBlockSets.empty());
    assert(!memoryBlockSets.front().first.empty());

    /** gathering data **/
    auto device = (*memoryBlockSets.front().first.begin())->GetDevice();

    assert(device);

    const auto & deviceConfig = device->DeviceConfig();
    const auto & protocolInfo = device->GetProtocolInfo();

    const bool isRead = IsReadOperation(operation);

    const bool enableHoles = (policy == Minify);

    TRegisterTypeInfo getMaxHoleAndRegs = [&](const TMemoryBlockType & type) {
        const bool singleBitType = protocolInfo.IsSingleBitType(type);

        const int maxHole = enableHoles ? (singleBitType ? deviceConfig->MaxBitHole
                                                         : deviceConfig->MaxRegHole)
                                        : 0;
        int maxRegs;

        if (isRead) {
            const auto protocolMaximum = singleBitType ? protocolInfo.GetMaxReadBits()
                                                       : protocolInfo.GetMaxReadRegisters();

            if (deviceConfig->MaxReadRegisters > 0) {
                maxRegs = min((uint32_t)deviceConfig->MaxReadRegisters, protocolMaximum);
            } else {
                maxRegs = protocolMaximum;
            }
        } else {
            maxRegs = singleBitType ? protocolInfo.GetMaxWriteBits()
                                    : protocolInfo.GetMaxWriteRegisters();
        }

        return pair<uint32_t, uint32_t>{ maxHole, maxRegs };
    };

    auto addQuery = [&](TAssociatedMemoryBlockSet && memoryBlocks, TQueries & result) {
        const auto & chosenAddQuery = isRead ? AddQuery<TIRDeviceQuery>
                                             : AddQuery<TIRDeviceValueQuery>;

        return chosenAddQuery(move(memoryBlocks), result);
    };

    /** done gathering data **/

#ifdef WB_MQTT_SERIAL_VERBOSE_OUTPUT
    auto printSets = [&](const char * prefix){
        cout << prefix << ":\n" << PrintCollection(memoryBlockSets, [](ostream & s, const TAssociatedMemoryBlockList::value_type & mbs){
            s << "MEMORY BLOCKS: " << PrintCollection(mbs.first, [](ostream & s, const PMemoryBlock & mb) {
                s << mb->Address;
            });
            s << endl;
            s << "VREGS: " << PrintCollection(mbs.second, [](ostream & s, const PVirtualValue & val) {
                s << dynamic_pointer_cast<TVirtualRegister>(val)->Describe();
            });
        }, true, "\n---------------") << endl;
    };
#else
    #define printSets(...)
#endif

    printSets("BEFORE MERGE");
    MergeSets(memoryBlockSets, getMaxHoleAndRegs, policy);
    printSets("AFTER MERGE");

    TQueries result;

    for (auto & memoryBlockSet: memoryBlockSets) {
        addQuery(move(memoryBlockSet), result);
    }

    assert(!result.empty());

    return result;
}

void TIRDeviceQueryFactory::CheckSets(const TAssociatedMemoryBlockList & memoryBlockSets, const TRegisterTypeInfo & typeInfo)
{
    if (Global::Debug)
        cerr << "checking sets" << endl;

    try {
        for (const auto & memoryBlockSet: memoryBlockSets) {
            uint32_t maxHole;
            uint32_t maxRegs;

            tie(maxHole, maxRegs) = typeInfo(GetType(memoryBlockSet.first));

            auto hole = GetMaxHoleSize(memoryBlockSet.first);
            if (hole > maxHole) {
                throw TSerialDeviceException("max hole count exceeded (detected: " +
                    to_string(hole) +
                    ", max: " + to_string(maxHole) +
                    ", set: " + PrintCollection(memoryBlockSet.first, [](ostream & s, const PMemoryBlock & mb) {
                        s << mb->Address;
                    })  + ")"
                );
            }

            auto regCount = GetRegCount(memoryBlockSet.first);
            if (regCount > maxRegs) {
                throw TSerialDeviceException("max mb count exceeded (detected: " +
                    to_string(regCount) +
                    ", max: " + to_string(maxRegs) +
                    ", set: " + PrintCollection(memoryBlockSet.first, [](ostream & s, const PMemoryBlock & mb) {
                        s << mb->Address;
                    })  + ")"
                );
            }

            {   // check types
                auto typeIndex = GetType(memoryBlockSet.first).Index;
                auto size = GetSize(memoryBlockSet.first);

                for (const auto & mb: memoryBlockSet.first) {
                    if (mb->Type.Index != typeIndex) {
                        throw TSerialDeviceException("different memory block types in same set (set: "
                            + PrintCollection(memoryBlockSet.first, [](ostream & s, const PMemoryBlock & mb) {
                                s << mb->Address << " (type: " << mb->GetTypeName() << ")";
                            }) + ")"
                        );
                    }

                    if (mb->Size != size) {
                        throw TSerialDeviceException("different memory block sizes in same set (set: "
                            + PrintCollection(memoryBlockSet.first, [](ostream & s, const PMemoryBlock & mb) {
                                s << mb->Address << " (size: " << mb->Size << ")";
                            }) + ")"
                        );
                    }
                }
            }
        }
    } catch (const TSerialDeviceException & e) {
        throw TSerialDeviceException("unable to create queries for given register configuration: " + string(e.what()));
    }

    if (Global::Debug)
        cerr << "checking sets done" << endl;
}

/**
 * Following algorihm:
 *  1) tries to reduce number of sets in passed list
 *  2) ensures that maxHole and maxRegs are not exceeded
 *  3) allows same memory block to appear in different sets if those sets couldn't merge
 *     (same register will be read more than once during same cycle)
 *  4) doesn't split initial sets (registers that were in one set will stay in one set)
 */
void TIRDeviceQueryFactory::MergeSets(
    TAssociatedMemoryBlockList & memoryBlockSets,
    const TRegisterTypeInfo & typeInfo,
    EQueryGenerationPolicy policy)
{
    CheckSets(memoryBlockSets, typeInfo);

    if (Global::Debug)
        cerr << "merging sets" << endl;

    // In case of minification, two sets may merge if:
    auto mergeCondition = [](
        const TPSet<PMemoryBlock> & a,
        const TPSet<PMemoryBlock> & b,
        const TRegisterTypeInfo & typeInfo)
    {
        if (GetType(a) != GetType(b)) { // Their memory blocks are of same type
            return false;
        }

        auto first = **a.begin() < **b.begin() ? *a.begin() : *b.begin();
        auto last = **b.rbegin() < **a.rbegin() ? *a.rbegin() : *b.rbegin();

        uint32_t maxHole, maxRegs; tie(maxHole, maxRegs) = typeInfo(GetType(a));

        auto holeAfterMerge = GetMaxHoleSize(first, last);
        auto regsAfterMerge = last->Address - first->Address + 1;

        return GetSize(a)     == GetSize(b) && // Their memory blocks are of same size
               holeAfterMerge <= maxHole    && // Hole after merge won't exceed limit
               regsAfterMerge <= maxRegs;      // Memory block count after merge won't exceed limit
    };

    // In case of memory block sets preservation, two sets may merge if:
    auto noDuplicatesCondition = [](
        const TPSet<PMemoryBlock> & a,
        const TPSet<PMemoryBlock> & b,
        const TRegisterTypeInfo & typeInfo)
    {
        return a == b;  // They are the same
    };

    auto condition = (policy == NoDuplicates) ? noDuplicatesCondition : mergeCondition;

    for (auto itMemoryBlockSet = memoryBlockSets.begin(); itMemoryBlockSet != memoryBlockSets.end(); ++itMemoryBlockSet) {
        auto itOtherMemoryBlockSet = itMemoryBlockSet;
        ++itOtherMemoryBlockSet;

        for (;itOtherMemoryBlockSet != memoryBlockSets.end();) {
            assert(itMemoryBlockSet != itOtherMemoryBlockSet);

            if (!condition(itMemoryBlockSet->first, itOtherMemoryBlockSet->first, typeInfo)) {
                ++itOtherMemoryBlockSet;
                continue;
            }

            // merge memory blocks
            itMemoryBlockSet->first.insert(itOtherMemoryBlockSet->first.begin(), itOtherMemoryBlockSet->first.end());
            // merge virtual registers
            itMemoryBlockSet->second.insert(itMemoryBlockSet->second.end(), itOtherMemoryBlockSet->second.begin(), itOtherMemoryBlockSet->second.end());

            itOtherMemoryBlockSet = memoryBlockSets.erase(itOtherMemoryBlockSet);
        }
    }

    if (Global::Debug)
        cerr << "merging sets done" << endl;
}
