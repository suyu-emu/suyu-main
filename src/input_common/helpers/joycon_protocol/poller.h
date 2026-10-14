// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Based on dkms-hid-nintendo implementation, CTCaer joycon toolkit and dekuNukem reverse
// engineering https://github.com/nicman23/dkms-hid-nintendo/blob/master/src/hid-nintendo.c
// https://github.com/CTCaer/jc_toolkit
// https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering

#pragma once

#include <array>
#include <functional>
#include <span>

#include "input_common/helpers/joycon_protocol/joycon_types.h"

namespace InputCommon::Joycon {

// Handles input packages and triggers the corresponding input events
class JoyconPoller {
public:
    JoyconPoller(ControllerType device_type_, JoyStickCalibration left_stick_calibration_,
                 JoyStickCalibration right_stick_calibration_,
                 MotionCalibration motion_calibration_);

    void SetCallbacks(const JoyconCallbacks& callbacks_);

    /// Handles data from passive packages
    void ReadPassiveMode(std::span<u8> buffer);

    /// Handles data from active packages
    void ReadActiveMode(std::span<u8> buffer, const MotionStatus& motion_status,
                        const RingStatus& ring_status);

    /// Handles data from nfc or ir packages
    void ReadNfcIRMode(std::span<u8> buffer, const MotionStatus& motion_status);

    void UpdateColor(const Color& color);
    void UpdateRing(s16 value, const RingStatus& ring_status);
    void UpdateAmiibo(const Joycon::TagInfo& tag_info);
    void UpdateCamera(const std::vector<u8>& camera_data, IrsResolution format);

private:
    void UpdateActiveLeftPadInput(const InputReportActive& input,
                                  const MotionStatus& motion_status);
    void UpdateActiveRightPadInput(const InputReportActive& input,
                                   const MotionStatus& motion_status);
    void UpdateActiveProPadInput(const InputReportActive& input, const MotionStatus& motion_status);

    void UpdatePassiveLeftPadInput(const InputReportPassive& buffer);
    void UpdatePassiveRightPadInput(const InputReportPassive& buffer);
    void UpdatePassiveProPadInput(const InputReportPassive& buffer);

    /// Returns a calibrated joystick axis from raw axis data
    f32 GetAxisValue(u16 raw_value, JoyStickAxisCalibration calibration) const;

    /// Returns a digital joystick axis from passive axis data
    std::pair<f32, f32> GetPassiveAxisValue(PassivePadStick raw_value) const;

    /// Returns a calibrated accelerometer axis from raw motion data
    f32 GetAccelerometerValue(s16 raw, const MotionSensorCalibration& cal,
                              AccelerometerSensitivity sensitivity) const;

    /// Returns a calibrated gyro axis from raw motion data
    f32 GetGyroValue(s16 raw_value, const MotionSensorCalibration& cal,
                     GyroSensitivity sensitivity) const;

    /// Returns a raw motion value from a buffer
    s16 GetRawIMUValues(size_t sensor, size_t axis, const InputReportActive& input) const;

    /// Returns the number of IMU samples available in the current active report
    std::size_t GetMotionSampleCount() const;

    /// Returns one IMU sample of the current active report as motion data
    MotionData GetMotionInput(const InputReportActive& input, const MotionStatus& motion_status,
                              std::size_t sample) const;

    /// Hands every IMU sample of the report to `deliver`, paced at the sensor's 5ms interval
    void DeliverMotion(const InputReportActive& input, const MotionStatus& motion_status,
                       const std::function<void(MotionData&)>& deliver) const;

    ControllerType device_type{};

    // Device calibration
    JoyStickCalibration left_stick_calibration{};
    JoyStickCalibration right_stick_calibration{};
    MotionCalibration motion_calibration{};

    // Third IMU sample of the current active report. InputReportActive only holds two; the third
    // shares its bytes with the Ring-Con data, so it is only valid while the Ring-Con is off.
    std::array<s16, 6> third_motion_sample{};
    bool has_third_motion_sample{};

    JoyconCallbacks callbacks{};
};

} // namespace InputCommon::Joycon
