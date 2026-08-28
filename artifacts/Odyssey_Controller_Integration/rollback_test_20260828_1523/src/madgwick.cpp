#include "madgwick.h"

#include <cmath>

void MadgwickAhrs::Reset()
{
  q_ = Quaternion{};
}

void MadgwickAhrs::Update(float gx, float gy, float gz, float ax, float ay, float az, float dt)
{
  if (dt <= 0.f || dt > 0.05f)
    dt = 0.0005f;

  Quaternion q = q_;
  float recip_norm;
  float s0, s1, s2, s3;
  float q_dot1, q_dot2, q_dot3, q_dot4;
  float _2q0, _2q1, _2q2, _2q3, _4q0, _4q1, _4q2, _8q1, _8q2, q0q0, q1q1, q2q2, q3q3;

  q_dot1 = 0.5f * (-q.x * gx - q.y * gy - q.z * gz);
  q_dot2 = 0.5f * (q.w * gx + q.y * gz - q.z * gy);
  q_dot3 = 0.5f * (q.w * gy - q.x * gz + q.z * gx);
  q_dot4 = 0.5f * (q.w * gz + q.x * gy - q.y * gx);

  if (!((ax == 0.f) && (ay == 0.f) && (az == 0.f)))
  {
    recip_norm = ax * ax + ay * ay + az * az;
    recip_norm = 1.f / std::sqrt(recip_norm);
    ax *= recip_norm;
    ay *= recip_norm;
    az *= recip_norm;

    _2q0 = 2.f * q.w;
    _2q1 = 2.f * q.x;
    _2q2 = 2.f * q.y;
    _2q3 = 2.f * q.z;
    _4q0 = 4.f * q.w;
    _4q1 = 4.f * q.x;
    _4q2 = 4.f * q.y;
    _8q1 = 8.f * q.x;
    _8q2 = 8.f * q.y;
    q0q0 = q.w * q.w;
    q1q1 = q.x * q.x;
    q2q2 = q.y * q.y;
    q3q3 = q.z * q.z;

    s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
    s1 = _4q1 * q3q3 - _2q3 * ax + 4.f * q0q0 * q.x - _2q0 * ay - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
    s2 = 4.f * q0q0 * q.y + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
    s3 = 4.f * q1q1 * q.z - _2q1 * ax + 4.f * q2q2 * q.z - _2q2 * ay;

    recip_norm = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;
    recip_norm = 1.f / std::sqrt(recip_norm + 1e-12f);
    s0 *= recip_norm;
    s1 *= recip_norm;
    s2 *= recip_norm;
    s3 *= recip_norm;

    q_dot1 -= beta_ * s0;
    q_dot2 -= beta_ * s1;
    q_dot3 -= beta_ * s2;
    q_dot4 -= beta_ * s3;
  }

  q.w += q_dot1 * dt;
  q.x += q_dot2 * dt;
  q.y += q_dot3 * dt;
  q.z += q_dot4 * dt;

  recip_norm = 1.f / std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  q.w *= recip_norm;
  q.x *= recip_norm;
  q.y *= recip_norm;
  q.z *= recip_norm;
  q_ = q;
}
