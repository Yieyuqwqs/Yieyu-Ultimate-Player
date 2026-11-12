// [23] NDK ASensor：绕过 Java 直接读陀螺仪/旋转矢量，驱动 8D。
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
int  gyro_start(int hz);      // 200Hz，失败返回 <0（无传感器不算错误，静默降级）
void gyro_stop(void);
int  gyro_read(float* yawDeg, float* pitchDeg, float* rollDeg);  // 非阻塞快照
int  gyro_is_available(void);
#ifdef __cplusplus
}
#endif
