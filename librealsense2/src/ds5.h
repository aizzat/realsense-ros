// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#pragma once

#include "ds5-private.h"

#include "algo.h"
#include "error-handling.h"

const double TIMESTAMP_TO_MILLISECONS = 0.001;

namespace rsimpl2
{
    class ds5_camera;

#pragma pack (push, 1)
    struct uvc_header
    {
        byte            length;
        byte            info;
        uint32_t        timestamp;
        byte            source_clock[6];
    };

    struct metadata_header
    {
        uint32_t    metaDataID;
        uint32_t    size;
    };


    struct metadata_capture_timing
    {
        metadata_header  metaDataIdHeader;
        uint32_t    version;
        uint32_t    flag;
        int         frameCounter;
        uint32_t    opticalTimestamp;   //In millisecond unit
        uint32_t    readoutTime;        //The readout time in millisecond second unit
        uint32_t    exposureTime;       //The exposure time in millisecond second unit
        uint32_t    frameInterval ;     //The frame interval in millisecond second unit
        uint32_t    pipeLatency;        //The latency between start of frame to frame ready in USB buffer
    };

#pragma pack(pop)

    struct metadata
    {
       uvc_header header;
       metadata_capture_timing md_capture_timing;
    };

    class ds5_timestamp_reader_from_metadata : public frame_timestamp_reader
    {
       std::unique_ptr<frame_timestamp_reader> _backup_timestamp_reader;
       static const int pins = 2;
       std::vector<std::atomic<bool>> _has_metadata;
       bool started;
       mutable std::recursive_mutex _mtx;

    public:
        ds5_timestamp_reader_from_metadata(std::unique_ptr<frame_timestamp_reader> backup_timestamp_reader)
            :_backup_timestamp_reader(std::move(backup_timestamp_reader)), _has_metadata(pins)
        {
            reset();
        }

        bool has_metadata(const request_mapping& mode, const void * metadata, size_t metadata_size)
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);

            if(metadata == nullptr || metadata_size == 0)
            {
                return false;
            }

            for(uint32_t i=0; i<metadata_size; i++)
            {
                if(((byte*)metadata)[i] != 0)
                {
                    return true;
                }
            }
            return false;
        }

        rs2_time_t get_frame_timestamp(const request_mapping& mode, const uvc::frame_object& fo) override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto pin_index = 0;
            if (mode.pf->fourcc == 0x5a313620) // Z16
                pin_index = 1;

            if(!_has_metadata[pin_index])
            {
               _has_metadata[pin_index] = has_metadata(mode, fo.metadata, fo.metadata_size);
            }

            if(_has_metadata[pin_index])
            {
                auto md = (metadata*)(fo.metadata);
                return (double)(md->header.timestamp)*TIMESTAMP_TO_MILLISECONS;
            }
            else
            {
                if (!started)
                {
                    LOG_WARNING("UVC timestamp not found! please apply UVC metadata patch.");
                    started = true;
                }
                return _backup_timestamp_reader->get_frame_timestamp(mode, fo);
            }
        }

        unsigned long long get_frame_counter(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto pin_index = 0;
            if (mode.pf->fourcc == 0x5a313620) // Z16
                pin_index = 1;

            if(_has_metadata[pin_index])
            {
                auto md = (metadata*)(fo.metadata);
                return md->md_capture_timing.frameCounter;
            }
            else
            {
                return _backup_timestamp_reader->get_frame_counter(mode, fo);
            }
        }

        void reset() override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            started = false;
            for (auto i = 0; i < pins; ++i)
            {
                _has_metadata[i] = false;
            }
        }

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto pin_index = 0;
            if (mode.pf->fourcc == 0x5a313620) // Z16
                pin_index = 1;

            return _has_metadata[pin_index] ? RS2_TIMESTAMP_DOMAIN_HARDWARE_CLOCK :
                                              _backup_timestamp_reader->get_frame_timestamp_domain(mode,fo);
        }
    };

    class ds5_timestamp_reader : public frame_timestamp_reader
    {
        static const int pins = 2;
        mutable std::vector<int64_t> counter;
        std::shared_ptr<uvc::time_service> _ts;
        mutable std::recursive_mutex _mtx;
    public:
        ds5_timestamp_reader(std::shared_ptr<uvc::time_service> ts)
            : counter(pins), _ts(ts)
        {
            reset();
        }

        void reset() override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            for (auto i = 0; i < pins; ++i)
            {
                counter[i] = 0;
            }
        }

        rs2_time_t get_frame_timestamp(const request_mapping& mode, const uvc::frame_object& fo) override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            return _ts->get_time();
        }

        unsigned long long get_frame_counter(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            auto pin_index = 0;
            if (mode.pf->fourcc == 0x5a313620) // Z16
                pin_index = 1;

            return ++counter[pin_index];
        }

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            return RS2_TIMESTAMP_DOMAIN_SYSTEM_TIME;
        }
    };

    class ds5_iio_hid_timestamp_reader : public frame_timestamp_reader
    {
        static const int sensors = 2;
        bool started;
        mutable std::vector<int64_t> counter;
        mutable std::recursive_mutex _mtx;
    public:
        ds5_iio_hid_timestamp_reader()
        {
            counter.resize(sensors);
            reset();
        }

        void reset() override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            started = false;
            for (auto i = 0; i < sensors; ++i)
            {
                counter[i] = 0;
            }
        }

        rs2_time_t get_frame_timestamp(const request_mapping& mode, const uvc::frame_object& fo) override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);

            if(has_metadata(mode, fo.metadata, fo.metadata_size))
            {
                auto timestamp = *((uint64_t*)((const uint8_t*)fo.metadata));
                return static_cast<rs2_time_t>(timestamp) * TIMESTAMP_TO_MILLISECONS;
            }

            if (!started)
            {
                LOG_WARNING("HID timestamp not found! please apply HID patch.");
                started = true;
            }

            return std::chrono::duration<rs2_time_t, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        bool has_metadata(const request_mapping& mode, const void * metadata, size_t metadata_size) const
        {
            if(metadata != nullptr && metadata_size > 0)
            {
                return true;
            }
            return false;
        }

        unsigned long long get_frame_counter(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            int index = 0;
            if (mode.pf->fourcc == 'GYRO')
                index = 1;

            return ++counter[index];
        }

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & mode, const uvc::frame_object& fo) const
        {
            if(has_metadata(mode ,fo.metadata, fo.metadata_size))
            {
                return RS2_TIMESTAMP_DOMAIN_HARDWARE_CLOCK;
            }
            return RS2_TIMESTAMP_DOMAIN_SYSTEM_TIME;
        }
    };

    class ds5_custom_hid_timestamp_reader : public frame_timestamp_reader
    {
        static const int sensors = 4; // TODO: implement frame-counter for each GPIO or
                                      //       reading counter field report
        mutable std::vector<int64_t> counter;
        mutable std::recursive_mutex _mtx;
    public:
        ds5_custom_hid_timestamp_reader()
        {
            counter.resize(sensors);
            reset();
        }

        void reset() override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            for (auto i = 0; i < sensors; ++i)
            {
                counter[i] = 0;
            }
        }

        rs2_time_t get_frame_timestamp(const request_mapping& /*mode*/, const uvc::frame_object& fo) override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            static const uint8_t timestamp_offset = 17;

            auto timestamp = *((uint64_t*)((const uint8_t*)fo.pixels + timestamp_offset));
            return static_cast<rs2_time_t>(timestamp) * TIMESTAMP_TO_MILLISECONS;

            return std::chrono::duration<rs2_time_t, std::milli>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        bool has_metadata(const request_mapping& /*mode*/, const void * /*metadata*/, size_t /*metadata_size*/) const
        {
            return true;
        }

        unsigned long long get_frame_counter(const request_mapping & mode, const uvc::frame_object& fo) const override
        {
            std::lock_guard<std::recursive_mutex> lock(_mtx);
            return ++counter.front();
        }

        rs2_timestamp_domain get_frame_timestamp_domain(const request_mapping & /*mode*/, const uvc::frame_object& /*fo*/) const
        {
            return RS2_TIMESTAMP_DOMAIN_HARDWARE_CLOCK;
        }
    };

    class ds5_info : public device_info
    {
    public:
        std::shared_ptr<device> create(const uvc::backend& backend) const override;

        uint8_t get_subdevice_count() const override
        {
            auto depth_pid = _depth.front().pid;
            switch(depth_pid)
            {
            case ds::RS400P_PID:
            case ds::RS410A_PID:
            case ds::RS430C_PID:
            case ds::RS440P_PID: return 1;
            case ds::RS420R_PID: return 2;
            case ds::RS450T_PID: return 3;
            default:
                throw not_implemented_exception(to_string() <<
                    "get_subdevice_count is not implemented for DS5 device of type " <<
                    depth_pid);
            }
        }

        ds5_info(std::shared_ptr<uvc::backend> backend,
                 std::vector<uvc::uvc_device_info> depth,
                 std::vector<uvc::usb_device_info> hwm,
                 std::vector<uvc::hid_device_info> hid)
            : device_info(std::move(backend)), _hwm(std::move(hwm)),
              _depth(std::move(depth)), _hid(std::move(hid)) {}

        static std::vector<std::shared_ptr<device_info>> pick_ds5_devices(
                std::shared_ptr<uvc::backend> backend,
                std::vector<uvc::uvc_device_info>& uvc,
                std::vector<uvc::usb_device_info>& usb,
                std::vector<uvc::hid_device_info>& hid);

    private:
        std::vector<uvc::uvc_device_info> _depth;
        std::vector<uvc::usb_device_info> _hwm;
        std::vector<uvc::hid_device_info> _hid;
    };

    class ds5_camera final : public device
    {
    public:
        class emitter_option : public uvc_xu_option<uint8_t>
        {
        public:
            const char* get_value_description(float val) const override
            {
                switch (static_cast<int>(val))
                {
                    case 0:
                    {
                        return "Off";
                    }
                    case 1:
                    {
                        return "On";
                    }
                    case 2:
                    {
                        return "Auto";
                    }
                    default:
                        throw invalid_value_exception("value not found");
                }
            }

            explicit emitter_option(uvc_endpoint& ep)
                : uvc_xu_option(ep, ds::depth_xu, ds::DS5_DEPTH_EMITTER_ENABLED,
                                "Power of the DS5 projector, 0 meaning projector off, 1 meaning projector on, 2 meaning projector in auto mode")
            {}
        };

        class asic_and_projector_temperature_options : public option
        {
        public:
            bool is_read_only() const override { return true; }

            void set(float) override
            {
                throw not_implemented_exception(to_string() << rs2_option_to_string(_option) << " is read-only!");
            }

            float query() const override
            {
                if (!is_enabled())
                    throw wrong_api_call_sequence_exception("query option is allow only in streaming!");

                #pragma pack(push, 1)
                struct temperature
                {
                    uint8_t is_projector_valid;
                    uint8_t is_asic_valid;
                    int8_t projector_temperature;
                    int8_t asic_temperature;
                };
                #pragma pack(pop)

                auto temperature_data = static_cast<temperature>(_ep.invoke_powered(
                    [this](uvc::uvc_device& dev)
                    {
                        temperature temp{};
                        if (!dev.get_xu(ds::depth_xu,
                                        ds::DS5_ASIC_AND_PROJECTOR_TEMPERATURES,
                                        reinterpret_cast<uint8_t*>(&temp),
                                        sizeof(temperature)))
                         {
                                throw invalid_value_exception(to_string() << "get_xu(...) failed!" << " Last Error: " << strerror(errno));
                         }

                        return temp;
                    }));

                int8_t temperature::* feild;
                uint8_t temperature::* is_valid_feild;

                switch (_option)
                {
                case RS2_OPTION_ASIC_TEMPERATURE:
                    feild = &temperature::asic_temperature;
                    is_valid_feild = &temperature::is_asic_valid;
                    break;
                case RS2_OPTION_PROJECTOR_TEMPERATURE:
                    feild = &temperature::projector_temperature;
                    is_valid_feild = &temperature::is_projector_valid;
                    break;
                default:
                    throw invalid_value_exception(to_string() << rs2_option_to_string(_option) << " is not temperature option!");
                }

                if (!static_cast<bool>(temperature_data.*is_valid_feild))
                    throw invalid_value_exception(to_string() << rs2_option_to_string(_option) << " value is not valid!");

                return temperature_data.*feild;
            }

            option_range get_range() const override
            {
                return option_range{-40, 125, 0, 0};
            }

            bool is_enabled() const override
            {
                return _ep.is_streaming();
            }

            const char* get_description() const override
            {
                switch (_option)
                {
                case RS2_OPTION_ASIC_TEMPERATURE:
                    return "Current Asic Temperature";
                case RS2_OPTION_PROJECTOR_TEMPERATURE:
                    return "Current Projector Temperature";
                default:
                    throw invalid_value_exception(to_string() << rs2_option_to_string(_option) << " is not temperature option!");
                }
            }

            explicit asic_and_projector_temperature_options(uvc_endpoint& ep, rs2_option opt)
                : _option(opt),
                  _ep(ep)
            {}

        private:
            uvc_endpoint&                 _ep;
            rs2_option                    _option;
        };

        class motion_module_temperature_option : public option
        {
        public:
            bool is_read_only() const override { return true; }

            void set(float) override
            {
                throw not_implemented_exception("option is read-only!");
            }

            float query() const override
            {
                if (!is_enabled())
                    throw wrong_api_call_sequence_exception("query option is allow only in streaming!");

                static const auto report_field = uvc::custom_sensor_report_field::value;
                auto data = _ep.get_custom_report_data(custom_sensor_name, report_name, report_field);
                if (data.empty())
                    throw invalid_value_exception("query() motion_module_temperature_option failed! Empty buffer arrived.");

                auto data_str = std::string(reinterpret_cast<char const*>(data.data()));
                return std::stof(data_str);
            }

            option_range get_range() const override
            {
                if (!is_enabled())
                    throw wrong_api_call_sequence_exception("get option range is allow only in streaming!");

                static const auto min_report_field = uvc::custom_sensor_report_field::minimum;
                static const auto max_report_field = uvc::custom_sensor_report_field::maximum;
                auto min_data = _ep.get_custom_report_data(custom_sensor_name, report_name, min_report_field);
                if (min_data.empty())
                    throw invalid_value_exception("get_range() motion_module_temperature_option failed! Empty buffer arrived.");

                auto max_data = _ep.get_custom_report_data(custom_sensor_name, report_name, max_report_field);
                if (max_data.empty())
                    throw invalid_value_exception("get_range() motion_module_temperature_option failed! Empty buffer arrived.");

                auto min_str = std::string(reinterpret_cast<char const*>(min_data.data()));
                auto max_str = std::string(reinterpret_cast<char const*>(max_data.data()));

                return option_range{std::stof(min_str),
                                    std::stof(max_str),
                                    0, 0};
            }

            bool is_enabled() const override
            {
                return _ep.is_streaming();
            }

            const char* get_description() const override
            {
                return "Current Motion-Module Temperature";
            }

            explicit motion_module_temperature_option(hid_endpoint& ep)
                : _ep(ep)
            {}

        private:
            const std::string custom_sensor_name = "custom";
            const std::string report_name = "data-field-custom-usage";
            hid_endpoint& _ep;
        };

        class enable_auto_exposure_option : public option
        {
        public:
            void set(float value) override
            {
                if (value <0 ) throw invalid_value_exception("Invalid Auto-Exposure mode request " + std::to_string(value));

                auto auto_exposure_prev_state = _auto_exposure_state->get_enable_auto_exposure();
                _auto_exposure_state->set_enable_auto_exposure(0.f < std::fabs(value));

                if (_auto_exposure_state->get_enable_auto_exposure()) // auto_exposure current value
                {
                    if (!auto_exposure_prev_state) // auto_exposure previous value
                    {
                        _to_add_frames = true; // auto_exposure moved from disable to enable
                    }
                }
                else
                {
                    if (auto_exposure_prev_state)
                    {
                        _to_add_frames = false; // auto_exposure moved from enable to disable
                    }
                }
            }

            float query() const override
            {
                return _auto_exposure_state->get_enable_auto_exposure();
            }

            option_range get_range() const override
            {
                return option_range{0, 1, 1, 1};
            }

            bool is_enabled() const override { return true; }

            const char* get_description() const override
            {
                return "Enable/disable auto-exposure";
            }

            enable_auto_exposure_option(uvc_endpoint* fisheye_ep,
                                        std::shared_ptr<auto_exposure_mechanism> auto_exposure,
                                        std::shared_ptr<auto_exposure_state> auto_exposure_state)
                : _auto_exposure_state(auto_exposure_state),
                  _to_add_frames((_auto_exposure_state->get_enable_auto_exposure())),
                  _auto_exposure(auto_exposure)
            {
                fisheye_ep->register_on_before_frame_callback(
                            [this](rs2_stream stream, rs2_frame& f, callback_invocation_holder callback)
                {
                    if (!_to_add_frames || stream != RS2_STREAM_FISHEYE)
                        return;

                    _auto_exposure->add_frame(f.get()->get_owner()->clone_frame(&f), std::move(callback));
                });
            }

        private:
            std::shared_ptr<auto_exposure_state>         _auto_exposure_state;
            std::atomic<bool>                            _to_add_frames;
            std::shared_ptr<auto_exposure_mechanism>     _auto_exposure;
        };

        class auto_exposure_mode_option : public option
        {
        public:
            auto_exposure_mode_option(std::shared_ptr<auto_exposure_mechanism> auto_exposure,
                                      std::shared_ptr<auto_exposure_state> auto_exposure_state)
                : _auto_exposure_state(auto_exposure_state),
                  _auto_exposure(auto_exposure)
            {}

            void set(float value) override
            {
                _auto_exposure_state->set_auto_exposure_mode(static_cast<auto_exposure_modes>((int)value));
                _auto_exposure->update_auto_exposure_state(*_auto_exposure_state);
            }

            float query() const override
            {
                return static_cast<float>(_auto_exposure_state->get_auto_exposure_mode());
            }

            option_range get_range() const override
            {
                return option_range{0, 2, 1, 0};
            }

            bool is_enabled() const override { return true; }

            const char* get_description() const override
            {
                return "Auto-Exposure mode";
            }

            const char* get_value_description(float val) const override
            {
                switch (static_cast<int>(val))
                {
                    case 0:
                    {
                        return "Static";
                    }
                    case 1:
                    {
                        return "Anti-Flicker";
                    }
                    case 2:
                    {
                        return "Hybrid";
                    }
                    default:
                        throw invalid_value_exception("value not found");
                }
            }

        private:
            std::shared_ptr<auto_exposure_state>        _auto_exposure_state;
            std::shared_ptr<auto_exposure_mechanism>    _auto_exposure;
        };

        class auto_exposure_antiflicker_rate_option : public option
        {
        public:
            auto_exposure_antiflicker_rate_option(std::shared_ptr<auto_exposure_mechanism> auto_exposure,
                                                  std::shared_ptr<auto_exposure_state> auto_exposure_state)
                : _auto_exposure_state(auto_exposure_state),
                  _auto_exposure(auto_exposure)
            {}

            void set(float value) override
            {
                _auto_exposure_state->set_auto_exposure_antiflicker_rate(static_cast<uint32_t>(value));
                _auto_exposure->update_auto_exposure_state(*_auto_exposure_state);
            }

            float query() const override
            {
                return static_cast<float>(_auto_exposure_state->get_auto_exposure_antiflicker_rate());
            }

            option_range get_range() const override
            {
                return option_range{50, 60, 10, 60};
            }

            bool is_enabled() const override { return true; }

            const char* get_description() const override
            {
                return "Auto-Exposure anti-flicker";
            }

            const char* get_value_description(float val) const override
            {
                switch (static_cast<int>(val))
                {
                    case 50:
                    {
                        return "50Hz";
                    }
                    case 60:
                    {
                        return "60Hz";
                    }
                    default:
                        throw invalid_value_exception("antiflicker_rate: get_value_description(...) failed. value not found!");
                }
            }

        private:
            std::shared_ptr<auto_exposure_state>         _auto_exposure_state;
            std::shared_ptr<auto_exposure_mechanism>     _auto_exposure;
        };

        std::shared_ptr<hid_endpoint> create_hid_device(const uvc::backend& backend,
                                                        const std::vector<uvc::hid_device_info>& all_hid_infos,
                                                        const firmware_version& camera_fw_version);

        std::shared_ptr<uvc_endpoint> create_depth_device(const uvc::backend& backend,
                                                          const std::vector<uvc::uvc_device_info>& all_device_infos);

        uvc_endpoint& get_depth_endpoint()
        {
            return static_cast<uvc_endpoint&>(get_endpoint(_depth_device_idx));
        }

        ds5_camera(const uvc::backend& backend,
            const std::vector<uvc::uvc_device_info>& dev_info,
            const std::vector<uvc::usb_device_info>& hwm_device,
            const std::vector<uvc::hid_device_info>& hid_info);

        std::vector<uint8_t> send_receive_raw_data(const std::vector<uint8_t>& input) override;
        virtual rs2_intrinsics get_intrinsics(unsigned int subdevice, stream_profile profile) const override;
        std::shared_ptr<auto_exposure_mechanism> register_auto_exposure_options(uvc_endpoint* uvc_ep, const uvc::extension_unit* fisheye_xu);

    private:
        bool is_camera_in_advanced_mode() const;

        const uint8_t _depth_device_idx;
        uint8_t _fisheye_device_idx;

        std::shared_ptr<hw_monitor> _hw_monitor;


        lazy<std::vector<uint8_t>> _coefficients_table_raw;
        lazy<std::vector<uint8_t>> _fisheye_intrinsics_raw;
        lazy<std::vector<uint8_t>> _fisheye_extrinsics_raw;

        std::vector<uint8_t> get_raw_calibration_table(ds::calibration_table_id table_id) const;
        std::vector<uint8_t> get_raw_fisheye_intrinsics_table() const;
        std::vector<uint8_t> get_raw_fisheye_extrinsics_table() const;
        pose get_device_position(unsigned int subdevice) const;

        // Bandwidth parameters from BOSCH BMI 055 spec'
        std::vector<std::pair<std::string, stream_profile>> sensor_name_and_hid_profiles =
            {{std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 200,  RS2_FORMAT_MOTION_RAW}},
             {std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 400,  RS2_FORMAT_MOTION_RAW}},
             {std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 1000, RS2_FORMAT_MOTION_RAW}},
             {std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 200,  RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 400,  RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("gyro_3d"),  {RS2_STREAM_GYRO,  1, 1, 1000, RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 125,  RS2_FORMAT_MOTION_RAW}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 250,  RS2_FORMAT_MOTION_RAW}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 500,  RS2_FORMAT_MOTION_RAW}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 1000, RS2_FORMAT_MOTION_RAW}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 125,  RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 250,  RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 500,  RS2_FORMAT_MOTION_XYZ32F}},
             {std::string("accel_3d"), {RS2_STREAM_ACCEL, 1, 1, 1000, RS2_FORMAT_MOTION_XYZ32F}},
             { "HID Sensor Class Device: Gyroscope", { RS2_STREAM_GYRO, 1, 1, 1000, RS2_FORMAT_MOTION_XYZ32F } } ,
             { "HID Sensor Class Device: Accelerometer", { RS2_STREAM_ACCEL, 1, 1, 1000, RS2_FORMAT_MOTION_XYZ32F } },
             { "HID Sensor Class Device: Custom", { RS2_STREAM_ACCEL, 1, 1, 1000, RS2_FORMAT_MOTION_XYZ32F } }
        };

        std::map<rs2_stream, std::map<unsigned, unsigned>> fps_and_sampling_frequency_per_rs2_stream =
                                                         {{RS2_STREAM_ACCEL, {{125,  1},
                                                                              {250,  4},
                                                                              {500,  5},
                                                                              {1000, 10}}},
                                                          {RS2_STREAM_GYRO,  {{200,  1},
                                                                              {400,  4},
                                                                              {1000, 10}}}};

        std::unique_ptr<polling_error_handler> _polling_error_handler;

         
    };

    class ds5_notification_decoder :public notification_decoder
    {
    public:
        notification decode(int value) override;
    };
}