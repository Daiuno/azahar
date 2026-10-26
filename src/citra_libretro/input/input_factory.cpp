// Copyright 2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <cmath>
#include <memory>
#include <unordered_map>
#include <libretro.h>

#include "common/math_util.h"
#include "common/vector_math.h"
#include "core/frontend/input.h"

#include "citra_libretro/core_settings.h"
#include "citra_libretro/environment.h"
#include "citra_libretro/input/input_factory.h"

namespace LibRetro {

namespace Input {

class LibRetroButtonFactory;
class LibRetroAxisFactory;
class LibRetroMotionFactory;

class LibRetroButton final : public ::Input::ButtonDevice {
public:
    explicit LibRetroButton(int joystick_, int button_) : joystick(joystick_), button(button_) {}

    bool GetStatus() const override {
        return CheckInput((unsigned int)joystick, RETRO_DEVICE_JOYPAD, 0, (unsigned int)button) > 0;
    }

private:
    int joystick;
    int button;
};

/// A button device factory that creates button devices from LibRetro joystick
class LibRetroButtonFactory final : public ::Input::Factory<::Input::ButtonDevice> {
public:
    /**
     * Creates a button device from a joystick button
     * @param params contains parameters for creating the device:
     *     - "joystick": the index of the joystick to bind
     *     - "button": the index of the button to bind
     */
    std::unique_ptr<::Input::ButtonDevice> Create(const Common::ParamPackage& params) override {
        const int joystick_index = params.Get("joystick", 0);

        const int button = params.Get("button", 0);
        return std::make_unique<LibRetroButton>(joystick_index, button);
    }
};

/// A axis device factory that creates axis devices from LibRetro joystick
class LibRetroAxis final : public ::Input::AnalogDevice {
public:
    explicit LibRetroAxis(int joystick_, int button_) : joystick(joystick_), button(button_) {}

    std::tuple<float, float> GetStatus() const override {
        auto axis_x =
            (float)CheckInput((unsigned int)joystick, RETRO_DEVICE_ANALOG, (unsigned int)button, 0);
        auto axis_y =
            (float)CheckInput((unsigned int)joystick, RETRO_DEVICE_ANALOG, (unsigned int)button, 1);
        return std::make_tuple(axis_x / INT16_MAX, -axis_y / INT16_MAX);
    }

private:
    int joystick;
    int button;
};

/// A axis device factory that creates axis devices from SDL joystick
class LibRetroAxisFactory final : public ::Input::Factory<::Input::AnalogDevice> {
public:
    /**
     * Creates a button device from a joystick button
     * @param params contains parameters for creating the device:
     *     - "joystick": the index of the joystick to bind
     *     - "button"(optional): the index of the button to bind
     *     - "hat"(optional): the index of the hat to bind as direction buttons
     *     - "axis"(optional): the index of the axis to bind
     *     - "direction"(only used for hat): the direction name of the hat to bind. Can be "up",
     *         "down", "left" or "right"
     *     - "threshould"(only used for axis): a float value in (-1.0, 1.0) which the button is
     *         triggered if the axis value crosses
     *     - "direction"(only used for axis): "+" means the button is triggered when the axis value
     *         is greater than the threshold; "-" means the button is triggered when the axis value
     *         is smaller than the threshold
     */
    std::unique_ptr<::Input::AnalogDevice> Create(const Common::ParamPackage& params) override {
        const int joystick_index = params.Get("joystick", 0);

        const int button = params.Get("axis", 0);
        return std::make_unique<LibRetroAxis>(joystick_index, button);
    }
};

/// Static sensor interface callbacks for LibRetro motion input
static retro_sensor_get_input_t sensor_get_input_callback = nullptr;
static retro_set_sensor_state_t sensor_set_state_callback = nullptr;
static bool gyro_enabled = false;
static bool accel_enabled = false;

// iOS CoreMotion is hardware-portrait: X right, Y earpiece, Z toward user.
// 3DS: X+ screen-left, Y+ toward user, Z+ screen-up.
static Common::Vec3<float> TransformMotionAxes(float in_x, float in_y, float in_z) {
    Common::Vec3<float> out;
    out.y = in_z;
    switch (LibRetro::settings.motion_rotation) {
    case 1: // landscapeLeft: home left, screen-up is -X
        out.x = -in_y;
        out.z = -in_x;
        break;
    case 2: // portrait upside down
        out.x = in_x;
        out.z = -in_y;
        break;
    case 3: // landscapeRight: home right, screen-up is +X
        out.x = in_y;
        out.z = in_x;
        break;
    default: // portrait
        out.x = -in_x;
        out.z = in_y;
        break;
    }
    return out;
}

/// Treat a ~40° recline (comfortable viewing) as 3DS rest: gravity (0, 0, -1).
static Common::Vec3<float> ApplyComfortPitch(Common::Vec3<float> v) {
    constexpr float kRad = 40.0f * 3.14159265f / 180.0f;
    const float c = std::cos(kRad);
    const float s = std::sin(kRad);
    return {v.x, v.y * c - v.z * s, v.y * s + v.z * c};
}

/// LibRetro motion device that implements 3DS gyroscope and accelerometer input
class LibRetroMotion final : public ::Input::MotionDevice {
public:
    explicit LibRetroMotion(int port_, float sensitivity_)
        : port(port_), sensitivity(sensitivity_) {
        InitSensors();
    }

    std::tuple<Common::Vec3<float>, Common::Vec3<float>> GetStatus() const override {
        Common::Vec3<float> accel = {0.0f, 0.0f, -1.0f}; // Default gravity pointing down
        Common::Vec3<float> gyro = {0.0f, 0.0f, 0.0f};   // Default no rotation

        if (sensor_get_input_callback) {
            if (accel_enabled) {
                float ios_x = sensor_get_input_callback(port, RETRO_SENSOR_ACCELEROMETER_X);
                float ios_y = sensor_get_input_callback(port, RETRO_SENSOR_ACCELEROMETER_Y);
                float ios_z = sensor_get_input_callback(port, RETRO_SENSOR_ACCELEROMETER_Z);
                auto transformed =
                    ApplyComfortPitch(TransformMotionAxes(ios_x, ios_y, ios_z));
                accel.x = transformed.x * sensitivity;
                accel.y = transformed.y * sensitivity;
                accel.z = transformed.z * sensitivity;
            }

            if (gyro_enabled) {
                // iOS CoreMotion is rad/s; 3DS HID expects deg/s.
                constexpr float RAD_TO_DEG = 180.0f / 3.14159265f;
                float ios_x = sensor_get_input_callback(port, RETRO_SENSOR_GYROSCOPE_X);
                float ios_y = sensor_get_input_callback(port, RETRO_SENSOR_GYROSCOPE_Y);
                float ios_z = sensor_get_input_callback(port, RETRO_SENSOR_GYROSCOPE_Z);
                auto transformed =
                    ApplyComfortPitch(TransformMotionAxes(ios_x, ios_y, ios_z));
                gyro.x = transformed.x * RAD_TO_DEG * sensitivity;
                gyro.y = transformed.y * RAD_TO_DEG * sensitivity;
                gyro.z = transformed.z * RAD_TO_DEG * sensitivity;
            }
        }

        return std::make_tuple(accel, gyro);
    }

private:
    int port;
    float sensitivity;

    void InitSensors() const {
        // Initialize sensors if not already done
        if (!sensor_get_input_callback || !sensor_set_state_callback) {
            struct retro_sensor_interface sensor_interface;
            if (LibRetro::GetSensorInterface(&sensor_interface)) {
                sensor_get_input_callback = sensor_interface.get_sensor_input;
                sensor_set_state_callback = sensor_interface.set_sensor_state;
            }
        }

        // Enable sensors at 60Hz rate (matching 3DS update frequency)
        const unsigned int event_rate = 60;

        if (sensor_set_state_callback) {
            if (!accel_enabled &&
                sensor_set_state_callback(port, RETRO_SENSOR_ACCELEROMETER_ENABLE, event_rate)) {
                accel_enabled = true;
            }
            if (!gyro_enabled &&
                sensor_set_state_callback(port, RETRO_SENSOR_GYROSCOPE_ENABLE, event_rate)) {
                gyro_enabled = true;
            }
        }
    }
};

/// Motion device factory that creates motion devices from LibRetro sensor interface
class LibRetroMotionFactory final : public ::Input::Factory<::Input::MotionDevice> {
public:
    /**
     * Creates a motion device from LibRetro sensor interface
     * @param params contains parameters for creating the device:
     *     - "port": the controller port to read motion from (default 0)
     *     - "sensitivity": motion sensitivity multiplier (default 1.0)
     */
    std::unique_ptr<::Input::MotionDevice> Create(const Common::ParamPackage& params) override {
        const int port = params.Get("port", 0);
        const float sensitivity = params.Get("sensitivity", 1.0f);
        return std::make_unique<LibRetroMotion>(port, sensitivity);
    }
};

void Init() {
    using namespace ::Input;
    RegisterFactory<ButtonDevice>("libretro", std::make_shared<LibRetroButtonFactory>());
    RegisterFactory<AnalogDevice>("libretro", std::make_shared<LibRetroAxisFactory>());
    RegisterFactory<MotionDevice>("libretro", std::make_shared<LibRetroMotionFactory>());
}

void Shutdown() {
    using namespace ::Input;
    UnregisterFactory<ButtonDevice>("libretro");
    UnregisterFactory<AnalogDevice>("libretro");
    UnregisterFactory<MotionDevice>("libretro");

    // Disable sensors on shutdown
    if (sensor_set_state_callback) {
        sensor_set_state_callback(0, RETRO_SENSOR_ACCELEROMETER_DISABLE, 60);
        sensor_set_state_callback(0, RETRO_SENSOR_GYROSCOPE_DISABLE, 60);
        sensor_get_input_callback = nullptr;
        sensor_set_state_callback = nullptr;
        accel_enabled = false;
        gyro_enabled = false;
    }
}

} // namespace Input
} // namespace LibRetro
