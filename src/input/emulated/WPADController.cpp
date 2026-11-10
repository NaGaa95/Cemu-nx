#include <api/Controller.h>
#include "input/emulated/WPADController.h"

#include "input/emulated/ClassicController.h"
#include "input/emulated/ProController.h"
#include "input/emulated/WiimoteController.h"

WPADController::WPADController(size_t player_index, WPADDataFormat data_format)
	: EmulatedController(player_index), m_data_format(data_format)
{
}

namespace
{
	bool HasDpdData(WPADDataFormat format)
	{
		switch (format)
		{
		case kDataFormat_CORE_ACC_DPD:
		case kDataFormat_FREESTYLE_ACC_DPD:
		case kDataFormat_CLASSIC_ACC_DPD:
		case kDataFormat_CORE_ACC_DPD_FULL:
		case kDataFormat_MPLS:
			return true;
		default:
			return false;
		}
	}

	// right turn is positive, dir is expressed in X left, Y up, Z towards the TV
	float GetMplsHeading(const WPADController::MplsDir& dir)
	{
		const glm::vec3& forward = dir[2];
		if (forward.x * forward.x + forward.z * forward.z >= 0.25f)
			return std::atan2(-forward.x, forward.z);
		return std::atan2(dir[0].z, dir[0].x);
	}

	void RotateMplsHeading(WPADController::MplsDir& dir, float angle)
	{
		const float c = std::cos(angle);
		const float s = std::sin(angle);
		for (auto& v : dir)
			v = {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
	}
}

void WPADController::reset_mpls()
{
	std::scoped_lock lock(m_mpls_mutex);
	m_mpls_pending_angle = glm::vec3{};
	m_mpls_pending_heading = 0.0f;
}

void WPADController::set_mpls_angle(const glm::vec3& angle)
{
	std::scoped_lock lock(m_mpls_mutex);
	m_mpls_pending_angle = angle;
}

void WPADController::set_mpls_direction(const MplsDir& dir)
{
	std::scoped_lock lock(m_mpls_mutex);
	m_mpls_pending_heading = GetMplsHeading(dir);
}

void WPADController::apply_mpls_reference(glm::vec3& angle, MplsDir& dir)
{
	std::scoped_lock lock(m_mpls_mutex);
	if (m_mpls_pending_angle)
	{
		m_mpls_angle_offset = angle - *m_mpls_pending_angle;
		m_mpls_pending_angle.reset();
	}
	angle -= m_mpls_angle_offset;
	if (m_mpls_pending_heading)
	{
		m_mpls_heading_offset = GetMplsHeading(dir) - *m_mpls_pending_heading;
		m_mpls_pending_heading.reset();
	}
	RotateMplsHeading(dir, m_mpls_heading_offset);
}

glm::vec3 WPADController::get_kpad_acc(MotionSample& sample) const
{
	bool nativeAxes = false;
	{
		std::shared_lock lock(m_mutex);
		for (const auto& controller : m_controllers)
		{
			if (controller->use_motion())
			{
				nativeAxes = controller->api() == InputAPI::Wiimote;
				break;
			}
		}
	}
	// only native Wii Remote samples already use the KPAD axes
	glm::vec3 acc;
	if (nativeAxes)
		sample.getAccelerometer(&acc[0]);
	else
		sample.getVPADAccelerometer(&acc[0]);
	return acc;
}

WPADDataFormat WPADController::get_default_data_format() const
{
	switch (get_device_type())
	{
	case kWAPDevCore:
		return kDataFormat_CORE_ACC_DPD;
	case kWAPDevFreestyle:
		return kDataFormat_FREESTYLE_ACC;
	case kWAPDevClassic:
		return kDataFormat_CLASSIC;
	case kWAPDevMPLS:
	case kWAPDevMPLSFreeStyle:
		return kDataFormat_MPLS;
	case kWAPDevMPLSClassic:
		return kDataFormat_CLASSIC_ACC_DPD;
	case kWAPDevURCC:
		return kDataFormat_URCC;
	default:
		return kDataFormat_CORE;
	}
}

uint32 WPADController::get_emulated_button_flag(WPADDataFormat format, uint32 id) const
{
	switch(format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	case kDataFormat_MPLS:
		return WiimoteController::s_get_emulated_button_flag(id);
	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
		return ClassicController::s_get_emulated_button_flag(id);
	
	case kDataFormat_TRAIN: break;
	case kDataFormat_GUITAR: break;
	case kDataFormat_BALANCE_CHECKER: break;
	case kDataFormat_DRUM: break;
	
	case kDataFormat_TAIKO: break;
	case kDataFormat_URCC:
		return ProController::s_get_emulated_button_flag(id);

	}

	return 0;
}

void WPADController::WPADRead(WPADStatus_t* status)
{
	controllers_update_states();
	uint32 button = 0;
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			button |= value;
		}
	}

	m_homebutton_down |= is_home_down();

	// todo fill position api from wiimote

	switch (m_data_format)
	{
	case kDataFormat_CORE:
	case kDataFormat_CORE_ACC:
	case kDataFormat_CORE_ACC_DPD:
	case kDataFormat_CORE_ACC_DPD_FULL:
	{
		memset(status, 0x00, sizeof(*status));
		status->button = button;
		break;
	}

	case kDataFormat_FREESTYLE:
	case kDataFormat_FREESTYLE_ACC:
	case kDataFormat_FREESTYLE_ACC_DPD:
	{
		WPADFSStatus_t* ex_status = (WPADFSStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->button = button;

		auto axis = get_axis();
		axis *= 127.0f;
		ex_status->fsStickX = (sint8)axis.x;
		ex_status->fsStickY = (sint8)axis.y;
		break;
	}

	case kDataFormat_CLASSIC:
	case kDataFormat_CLASSIC_ACC:
	case kDataFormat_CLASSIC_ACC_DPD:
	case kDataFormat_GUITAR:
	case kDataFormat_DRUM:
	case kDataFormat_TAIKO:
	{
		WPADCLStatus_t* ex_status = (WPADCLStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->clButton = button;
		
		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->clLStickX = (uint16)axis.x;
		ex_status->clLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->clRStickX = (uint16)rotation.x;
		ex_status->clRStickY = (uint16)rotation.y;
		break;
	}
	case kDataFormat_TRAIN:
	{
		WPADTRStatus_t* ex_status = (WPADTRStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_BALANCE_CHECKER:
	{
		WPADBLStatus_t* ex_status = (WPADBLStatus_t*)status;
		// TODO
		break;
	}
	case kDataFormat_MPLS:
	{
		WPADMPStatus_t* ex_status = (WPADMPStatus_t*)status;
		memset(ex_status, 0, sizeof(*ex_status));
		ex_status->button = button;
		if (get_mpls_mode() == 0)
			break;
		ex_status->stat = 0x80;
		glm::vec3 gyro{};
		if (has_motion())
		{
			auto sample = get_motion_data();
			sample.getGyrometer(&gyro[0]);
		}
		const auto encode = [&](float radians, uint8 slowFlag) {
			constexpr float toDegrees = 180.0f / 3.14159265358979323846f;
			constexpr float slowScale = (0x1100 / 270.0f) * toDegrees;
			constexpr float fastScale = (0x1100 / 1200.0f) * toDegrees;
			if (!std::isfinite(radians))
				radians = 0;
			const bool slow = std::abs(radians) < 8192.0f / slowScale;
			if (slow)
				ex_status->stat |= slowFlag;
			const float value = std::clamp(8192.0f + radians * (slow ? slowScale : fastScale), 0.0f, 16383.0f);
			return static_cast<uint16>(std::lround(value));
		};
		ex_status->pitch = encode(-gyro.x, 0x04);
		ex_status->yaw = encode(-gyro.y, 0x08);
		ex_status->roll = encode(gyro.z, 0x02);
		if (get_device_type() == kWAPDevMPLSFreeStyle)
		{
			ex_status->stat |= 0x41;
			const auto axis = get_axis() * 127.0f;
			ex_status->status.fs.fsStickX = static_cast<sint8>(axis.x);
			ex_status->status.fs.fsStickY = static_cast<sint8>(axis.y);
			ex_status->status.fs.fsAccZ = 99;
		}
		break;
	}
	case kDataFormat_URCC:
	{
		WPADUCStatus_t* ex_status = (WPADUCStatus_t*)status;
		memset(ex_status, 0x00, sizeof(*ex_status));
		ex_status->ucButton = button;

		ex_status->cable = TRUE;
		ex_status->charge = TRUE;

		auto axis = get_axis();
		axis *= 2048.0f;
		ex_status->ucLStickX = (uint16)axis.x;
		ex_status->ucLStickY = (uint16)axis.y;

		auto rotation = get_rotation();
		rotation *= 2048.0f;
		ex_status->ucRStickX = (uint16)rotation.x;
		ex_status->ucRStickY = (uint16)rotation.y;

		break;
	}
	default:
		cemu_assert(false);
	}

	if (type() == Wiimote && has_motion())
	{
		auto sample = get_motion_data();
		const glm::vec3 acc = get_kpad_acc(sample);
		const auto encode = [](float value) {
			if (!std::isfinite(value))
				value = 0;
			return static_cast<uint16>(static_cast<sint16>(std::lround(std::clamp(value * 99.0f, -32768.0f, 32767.0f))));
		};
		status->accX = encode(-acc.x);
		status->accY = encode(acc.z);
		status->accZ = encode(-acc.y);
	}
	if (HasDpdData(m_data_format) && has_position() && GetPositionVisibility() != PositionVisibility::NONE)
	{
		const auto position = get_position();
		const float x = (1.0f - position.x) * 1023.0f;
		const float y = std::clamp(position.y * 767.0f, 0.0f, 767.0f);
		for (int i = 0; i < 2; ++i)
		{
			auto& obj = status->obj[i];
			obj.x = static_cast<uint16>(std::clamp(x + (i == 0 ? -64.0f : 64.0f), 0.0f, 1023.0f));
			obj.y = static_cast<uint16>(y);
			obj.size = 2;
			obj.traceId = static_cast<uint8>(i);
		}
	}
	status->dev = get_device_type();
	status->err = WPAD_ERR_NONE;
}

void WPADController::KPADRead(KPADStatus_t& status, const BtnRepeat& repeat)
{
	uint32be* hold, *release, *trigger;
	switch (type())
	{
	case Pro:
		hold = &status.ex_status.uc.hold;
		release = &status.ex_status.uc.release;
		trigger = &status.ex_status.uc.trig;
		break;
	case Classic:
		hold = &status.ex_status.cl.hold;
		release = &status.ex_status.cl.release;
		trigger = &status.ex_status.cl.trig;
		break;
	default:
		hold = &status.hold;
		release = &status.release;
		trigger = &status.trig;
	}

	controllers_update_states();
	for (uint32 i = 1; i < get_highest_mapping_id(); ++i)
	{
		if (is_mapping_down(i))
		{
			const uint32 value = get_emulated_button_flag(m_data_format, i);
			*hold |= value;
		}
	}

	m_homebutton_down |= is_home_down();
	
	// button repeat
	const auto now = std::chrono::steady_clock::now();
	if (*hold != m_last_holdvalue)
	{
		m_last_hold_change = m_last_pulse = now;
	}

	if (repeat.pulse > 0)
	{
		if (m_last_hold_change + std::chrono::milliseconds(repeat.delay) >= now)
		{
			if ((m_last_pulse + std::chrono::milliseconds(repeat.pulse)) < now)
			{
				m_last_pulse = now;
				*hold |= kWPADButtonRepeat;
			}
		}
	}

	// axis
	const auto axis = get_axis();
	const auto rotation = get_rotation();

	*release = m_last_holdvalue & ~*hold;
	//status.release = m_last_holdvalue & ~*hold;
	*trigger = ~m_last_holdvalue & *hold;
	//status.trig = ~m_last_holdvalue & *hold;
	m_last_holdvalue = *hold;

	const bool mplsEnabled = get_mpls_mode() != 0;
	glm::vec3 mplsVel{}, mplsAngle{};
	MplsDir mplsDir{glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}};

	if (has_motion())
	{
		auto motion_sample = get_motion_data();

		const glm::vec3 acc = get_kpad_acc(motion_sample);
		status.acc.x = acc.x;
		status.acc.y = acc.y;
		status.acc.z = acc.z;

		status.acc_value = motion_sample.getVPADAccMagnitude();
		status.acc_speed = motion_sample.getVPADAccAcceleration();

		//glm::vec2 acc_vert;
		//motion_sample.getVPADAccXY(&acc_vert[0]);
		//status.acc_vertical.x = acc_vert.x;
		//status.acc_vertical.y = acc_vert.y;

		status.accVertical.x = std::min(1.0f, std::abs(acc.x + acc.y));
		status.accVertical.y = std::min(std::max(-1.0f, -acc.z), 1.0f);

		if (mplsEnabled)
		{
			motion_sample.getVPADGyroChange(&mplsVel[0]);
			motion_sample.getVPADOrientation(&mplsAngle[0]);

			float attitude[9];
			motion_sample.getVPADAttitudeMatrix(attitude);
			for (int i = 0; i < 3; ++i)
				mplsDir[i] = {attitude[i * 3], attitude[i * 3 + 1], attitude[i * 3 + 2]};
			apply_mpls_reference(mplsAngle, mplsDir);
		}
	}
	if (mplsEnabled)
	{
		const auto store = [](beVec3D_t& out, const glm::vec3& v) {
			out.x = v.x;
			out.y = v.y;
			out.z = v.z;
		};
		store(status.mpls.mpls, mplsVel);
		store(status.mpls.angle, mplsAngle);
		store(status.mpls.dir.X, mplsDir[0]);
		store(status.mpls.dir.Y, mplsDir[1]);
		store(status.mpls.dir.Z, mplsDir[2]);
	}
	auto visibility = GetPositionVisibility();
	if (has_position() && visibility != PositionVisibility::NONE)
	{
		if (visibility == PositionVisibility::FULL)
			status.dpd_valid_fg = 2;
		else
			status.dpd_valid_fg = -1;

		status.horizon.x = 1.0f;
		status.dist = 1.0f;

		const auto position = get_position();

		const auto pos = (position * 2.0f) - 1.0f;
		status.pos.x = pos.x;
		status.pos.y = pos.y;

		const auto delta = position - get_prev_position();
		status.vec.x = delta.x;
		status.vec.y = delta.y;
		status.speed = glm::length(delta);
	}
	else
		status.dpd_valid_fg = 0;

	switch (type())
	{
	case Wiimote:
		status.ex_status.fs.stick.x = axis.x;
		status.ex_status.fs.stick.y = axis.y;

		if(has_second_motion())
		{
			auto motion_sample = get_second_motion_data();

			glm::vec3 acc;
			motion_sample.getAccelerometer(&acc[0]);
			status.ex_status.fs.acc.x = acc.x;
			status.ex_status.fs.acc.y = acc.y;
			status.ex_status.fs.acc.z = acc.z;

			status.ex_status.fs.accValue = motion_sample.getVPADAccMagnitude();
			status.ex_status.fs.accSpeed = motion_sample.getVPADAccAcceleration();
		}

		break;
	case Pro:
		status.ex_status.uc.lstick.x = axis.x;
		status.ex_status.uc.lstick.y = axis.y;

		status.ex_status.uc.rstick.x = rotation.x;
		status.ex_status.uc.rstick.y = rotation.y;

		status.ex_status.uc.charge = FALSE;
		status.ex_status.uc.cable = TRUE;

		break;
	case Classic:
		status.ex_status.cl.lstick.x = axis.x;
		status.ex_status.cl.lstick.y = axis.y;

		status.ex_status.cl.rstick.x = rotation.x;
		status.ex_status.cl.rstick.y = rotation.y;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZL))
			status.ex_status.cl.ltrigger = 1.0f;

		if (HAS_FLAG((uint32)status.ex_status.cl.hold, kCLButton_ZR))
			status.ex_status.cl.rtrigger = 1.0f;
		break;
	default:
		cemu_assert(false);
	}

	
}
