#pragma once

struct Quaternion
{
  float w = 1.f;
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
};

class MadgwickAhrs
{
public:
  void Reset();
  void Update(float gx, float gy, float gz, float ax, float ay, float az, float dt);
  void BlendMagYaw(float mx, float my, float mz, float alpha);
  Quaternion Orientation() const { return q_; }
  void SetBeta(float beta) { beta_ = beta; }

private:
  Quaternion q_{};
  float beta_ = 0.08f;
};
