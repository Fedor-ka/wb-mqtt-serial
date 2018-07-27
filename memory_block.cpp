#include "memory_block.h"
#include "serial_device.h"
#include "virtual_register.h"
#include "constraints.h"

#include <cassert>

using namespace std;


bool TMemoryBlock::InitExternalLinkage(const PSerialDevice & device)
{
    struct TSerialDeviceLinkage: TMemoryBlock::IExternalLinkage
    {
        PWSerialDevice Device;
        TMemoryBlock & Register;


        TSerialDeviceLinkage(const PSerialDevice & device, TMemoryBlock & self)
            : Device(device)
            , Register(self)
        {}

        PSerialDevice GetDevice() const override
        {
            auto device = Device.lock();
            assert(device);

            return device;
        }

        TPSet<PVirtualRegister> GetVirtualRegsiters() const override
        {
            return {};
        }

        void LinkWith(const PVirtualRegister &) override
        {
            assert(false);
        }

        bool IsLinkedWith(const PVirtualRegister &) const override
        {
            return false;
        }

        bool NeedsCaching() const override
        {
            return false;
        }
    };

    if (ExternalLinkage && dynamic_cast<TSerialDeviceLinkage*>(ExternalLinkage.get())) {
        return false;
    }

    ExternalLinkage = utils::make_unique<TSerialDeviceLinkage>(device, *this);

    return true;
}

bool TMemoryBlock::InitExternalLinkage(const PVirtualRegister & vreg)
{
    struct TVirtualRegisterLinkage: TMemoryBlock::IExternalLinkage
    {
        using PWMemoryBlock = weak_ptr<TMemoryBlock>;

        PWMemoryBlock             MemoryBlock;      // linkage gets destroyed with memory block - no need in shared_ptr
        TPWSet<PWVirtualRegister> VirtualRegisters;

        TVirtualRegisterLinkage(const PWMemoryBlock & memoryBlock, const PVirtualRegister & vreg)
            : MemoryBlock(memoryBlock)
        {
            LinkWithImpl(vreg);
        }

        PVirtualRegister AssociatedVirtualRegister() const
        {
            assert(!VirtualRegisters.empty());

            auto virtualReg = VirtualRegisters.begin()->lock();
            assert(virtualReg);

            return virtualReg;
        }

        bool Has(const PVirtualRegister & vreg) const
        {
            return find_if(VirtualRegisters.begin(), VirtualRegisters.end(), [&](const PWVirtualRegister & added){
                auto lockedAdded = added.lock();
                assert(lockedAdded);
                return lockedAdded == vreg;
            }) != VirtualRegisters.end();
        }

        void LinkWithImpl(const PVirtualRegister & vreg)
        {
            VirtualRegisters.insert(vreg);
        }

        TPSet<PVirtualRegister> GetVirtualRegsiters() const override
        {
            assert(!VirtualRegisters.empty());

            TPSet<PVirtualRegister> result;

            for (const auto & vreg: VirtualRegisters) {
                const auto & locked = vreg.lock();
                assert(locked);

                bool inserted = result.insert(locked).second;
                assert(inserted);
            }

            return result;
        }

        PSerialDevice GetDevice() const
        {
            assert(!VirtualRegisters.empty());

            return AssociatedVirtualRegister()->GetDevice();
        }

        void LinkWith(const PVirtualRegister & vreg) override
        {
            assert(!VirtualRegisters.empty());
            assert(GetDevice() == vreg->GetDevice());
            assert(AssociatedVirtualRegister()->Type == vreg->Type);
            assert(!Has(vreg));

            {   // check for overlapping
                auto itOtherReg = VirtualRegisters.find(vreg);

                if (itOtherReg != VirtualRegisters.end()) {
                    auto otherReg = itOtherReg->lock();
                    throw TSerialDeviceException(
                        "registers " + vreg->ToStringWithFormat() +
                        " and " + otherReg->ToStringWithFormat() + " are overlapping"
                    );
                }
            }

            LinkWithImpl(vreg);
        }

        bool IsLinkedWith(const PVirtualRegister & vreg) const override
        {
            assert(!VirtualRegisters.empty());

            return VirtualRegisters.count(vreg);
        }

        /**
         * @note If any of virtual registers is not covering block entierly and is writable,
         *  it means that there can be partial write of the memory block,
         *  so we need cache to avoid corruption of non-covered part of memory block at write.
         */
        bool NeedsCaching() const override
        {
            assert(!VirtualRegisters.empty());
            auto memoryBlock = MemoryBlock.lock();
            assert(memoryBlock);
            const TIRBindInfo fullCoverage {
                0, static_cast<uint16_t>(memoryBlock->Size * 8)
            };

            return AnyOf(VirtualRegisters, [&](const PWVirtualRegister & wvreg) {
                auto vreg = wvreg.lock();

                auto writable = !memoryBlock->Type.ReadOnly && !vreg->ReadOnly;
                auto notFullCoverage = vreg->GetMemoryBlockBindInfo(memoryBlock) != fullCoverage;

                return writable && notFullCoverage;
            });
        }
    };

    if (ExternalLinkage && dynamic_cast<TVirtualRegisterLinkage*>(ExternalLinkage.get())) {
        return false;
    }

    ExternalLinkage = utils::make_unique<TVirtualRegisterLinkage>(shared_from_this(), vreg);
    return true;
}

TMemoryBlock::TMemoryBlock(uint32_t address, uint16_t size, const TMemoryBlockType & type)
    : Cache(nullptr)
    , Address(address)
    , Type(type)
    , Size(type.IsVariadicSize() ? size : type.Size)
{
    assert(Size < MAX_MEMORY_BLOCK_SIZE && "memory block size doesn't fit constraints");
}

TMemoryBlock::TMemoryBlock(uint32_t address, const TMemoryBlockType & type)
    : TMemoryBlock(address, type.Size, type)
{
    assert(!type.IsVariadicSize() && "memory block with variadic size must be created with size specified");
}

TMemoryBlock::TMemoryBlock(uint32_t address, uint16_t size, uint32_t typeIndex, const PSerialDevice & device)
    : TMemoryBlock(
        address,
        size,
        device->Protocol()->GetRegType(typeIndex)
    )
{
    InitExternalLinkage(device);
}

TMemoryBlock::TMemoryBlock(uint32_t address, uint32_t typeIndex, const PSerialDevice & device)
    : TMemoryBlock(
        address,
        device->Protocol()->GetRegType(typeIndex)
    )
{
    InitExternalLinkage(device);
}

bool TMemoryBlock::operator<(const TMemoryBlock & rhs) const
{
    return Type.Index < rhs.Type.Index || (Type.Index == rhs.Type.Index && Address < rhs.Address);
}

bool TMemoryBlock::operator==(const TMemoryBlock & rhs) const
{
    if (this == &rhs) {
        return true;
    }

    return Type.Index == rhs.Type.Index && Address == rhs.Address && GetDevice() == rhs.GetDevice();
}

void TMemoryBlock::AssociateWith(const PVirtualRegister & vreg)
{
    if (!InitExternalLinkage(vreg)) {
        ExternalLinkage->LinkWith(vreg);
    }
}

bool TMemoryBlock::IsAssociatedWith(const PVirtualRegister & vreg) const
{
    assert(ExternalLinkage);

    return ExternalLinkage->IsLinkedWith(vreg);
}

bool TMemoryBlock::IsReady() const
{
    return bool(ExternalLinkage);
}

const string & TMemoryBlock::GetTypeName() const
{
    return Type.Name;
}

PSerialDevice TMemoryBlock::GetDevice() const
{
    assert(ExternalLinkage);

    return ExternalLinkage->GetDevice();
}

TPSet<PVirtualRegister> TMemoryBlock::GetVirtualRegsiters() const
{
    assert(ExternalLinkage);

    return ExternalLinkage->GetVirtualRegsiters();
}

bool TMemoryBlock::NeedsCaching() const
{
    return ExternalLinkage && ExternalLinkage->NeedsCaching();
}

void TMemoryBlock::AssignCache(uint8_t * cache)
{
    assert(NeedsCaching());
    assert(!Cache);

    Cache = cache;

    if (Global::Debug) {
        cerr << "Adding to cache " << Describe() << endl;
    }
}

TIRDeviceMemoryBlockView TMemoryBlock::GetCache() const
{
    assert(NeedsCaching() == bool(Cache));

    return { Cache, shared_from_this(), false };
}

std::string TMemoryBlock::Describe() const
{
    return GetTypeName() + " memory block " + to_string(Address) + " of device " + GetDevice()->ToString();
}
