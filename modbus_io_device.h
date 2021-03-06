#pragma once

#include "serial_device.h"

class TModbusIODevice : public TBasicProtocolSerialDevice<TBasicProtocol<TModbusIODevice, TAggregatedSlaveId>> {
public:
    TModbusIODevice(PDeviceConfig config, PPort port, PProtocol protocol);
    std::list<PRegisterRange> SplitRegisterList(const std::list<PRegister> & reg_list, bool enableHoles = true) const override;
    uint64_t ReadRegister(PRegister reg) override;
    void WriteRegister(PRegister reg, uint64_t value) override;
    void ReadRegisterRange(PRegisterRange range) override;

private:
    int Shift;
};
