/**
 * @file  t_hostext.c
 * @brief host_ext.c（上位机扩展功能胶水层）的主机测试
 *
 * 链接真 `App/Svc/host_ext.c` + `test/stub/host_appstub.c`（控制模块替身）。
 * 钉死的是**调用序列与安全策略**：
 *   - 驾驶三种模式各自该动哪些模块、顺序对不对；
 *   - 任务解释器在跑时一律拒绝（`TaskRun_IsRunning()` 由 host_taskrunstub.c 造）；
 *   - 500ms 链路看门狗（边界 499/500）；
 *   - 动作表读写、舵机夹限、固化请求落到 nvram_touch()。
 * 实机的控制行为不在这里验（那是台架/上板的事）。
 */
#include "t_common.h"

#include "host_ext.h"
#include "motion.h"
#include "servo.h"

/* 替身暴露的观测变量（host_appstub.c） */
extern int   stub_motion_mode;
extern int   stub_motion_speed;
extern int   stub_clear_faults;
extern int   stub_sc_enabled;
extern int   stub_sc_l, stub_sc_r;
extern float stub_sc_ramp;
extern int   stub_sc_stops;
extern int   stub_gyro_started, stub_gyro_stopped;
extern float stub_gyro_target;
extern float stub_gyro_target_cont;   /* ★2026-10-07 连续域目标（HOST_DRIVE_BASE 走这条） */
extern int   stub_servo_apply_n;
extern int   stub_servo_ctl[3];
extern int   stub_servo_dev[3];
extern float stub_servo_angle[3];
extern int   stub_nvram_touch;
extern uint8_t stub_nvram_state;
extern float Yaw, Pitch, Roll;

/* host_taskrunstub.c：任务解释器的替身（host_ext.c 只问 TaskRun_IsRunning / 低压时叫 Stop） */
extern uint8_t stubx_taskrun_running;
extern uint8_t stubx_taskrun_stops;

/* host_hal.c 的假时钟 */
extern void host_tick_set(uint32_t ms);

static void reset_all(void)
{
    stub_motion_mode = 0;
    stub_motion_speed = 0;
    stub_clear_faults = 0;
    stub_sc_enabled = 0;
    stub_sc_l = stub_sc_r = 0;
    stub_sc_ramp = 0.0f;
    stub_sc_stops = 0;
    stub_gyro_started = stub_gyro_stopped = 0;
    stub_gyro_target = 0.0f;
    stub_gyro_target_cont = 0.0f;
    stub_servo_apply_n = 0;
    stub_servo_ctl[1] = stub_servo_ctl[2] = 0;
    stub_servo_dev[1] = stub_servo_dev[2] = 0;
    stub_servo_angle[1] = stub_servo_angle[2] = 0.0f;
    stub_nvram_touch = 0;
    stub_nvram_state = 0;
    Yaw = Pitch = Roll = 0.0f;
    stubx_taskrun_running = 0u;
    host_tick_set(1000U);
    HostExt_Init();
}

int main(void)
{
    int a = 0, b = 0;

    /* ---- 1. 初值：关闭、未使能 ---- */
    reset_all();
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_OFF);
    CHECK_EQ_I(HostExt_DriveEnabled(), 0);
    CHECK_EQ_I(HostExt_DriveGet(&a, &b), 0);
    CHECK_EQ_I(HostExt_CurrentAction(), 0);

    /* ---- 2. 任务解释器在跑时拒绝远程驾驶 ---- */
    reset_all();
    stubx_taskrun_running = 1u;
    CHECK_EQ_I(HostExt_TaskRunning(), 1);    /* 遥测/页面的"车在忙"就是问这个 */
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_WHEELS, 300, 300), -2);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_OFF);
    CHECK_EQ_I(stub_sc_enabled, 0);          /* 一根手指头都没动过 */
    CHECK_EQ_I(stub_motion_mode, 0);
    stubx_taskrun_running = 0u;
    CHECK_EQ_I(HostExt_TaskRunning(), 0);

    /* ---- 3. WHEELS：速度环单环（先 IDLE，再设斜坡/目标/使能） ---- */
    reset_all();
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_WHEELS, 300, -300), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);
    CHECK(stub_sc_ramp == HOST_DRIVE_RAMP);
    CHECK_EQ_I(stub_sc_l, 300);
    CHECK_EQ_I(stub_sc_r, -300);
    CHECK_EQ_I(stub_sc_enabled, 1);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_WHEELS);
    CHECK_EQ_I(HostExt_DriveEnabled(), 1);
    CHECK_EQ_I(HostExt_MotorEnabled(), 1);        /* 电机带电：上位机要提示 */
    CHECK_EQ_I(HostExt_DriveGet(&a, &b), 1);
    CHECK_EQ_I(a, 300);
    CHECK_EQ_I(b, -300);
    CHECK_EQ_I(stub_motion_speed, 0);              /* WHEELS 不动基准速度 */

    /* ---- 4. BASE：角度环 + 速度环；基准航向只在"刚进模式"时锁存 ----
     * ★2026-10-07：`b` 是上位机的**累计转角**，走的是 `GyroControl_SetTargetAngleCont()`
     * （连续域、不取模）而不是 `SetTargetAngle(NormalizeAngle(...))` —— 取模 + "最近路"
     * 会在累计领先超过 180° 时把"继续往左"解释成"往右 160°"（真车实测翻号点
     * 183.4/201.5/203.9/209.0°）。所以这里钉的是 `stub_gyro_target_cont`。 */
    reset_all();
    Yaw = 10.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 5000, 30), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_GYRO);
    CHECK_EQ_I(stub_motion_speed, 5000);
    CHECK(stub_gyro_target_cont == 40.0f);         /* 10 + 30（基准 = 进模式时的连续域航向） */
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_BASE);

    Yaw = 50.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 4000, 10), 0);
    CHECK(stub_gyro_target_cont == 20.0f);         /* 基准仍是 10（50 只是当前航向） */
    CHECK_EQ_I(stub_motion_speed, 4000);

    /* 连续域**不折回**：170 + 40 = 210 就存 210（旧路会折成 −150，那正是"倒拐"的根因） */
    reset_all();
    Yaw = 170.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 3000, 40), 0);
    CHECK(stub_gyro_target_cont == 210.0f);

    /* ---- 5. TRACE：三级串级循迹 ---- */
    reset_all();
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_TRACE, 4000, 0), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_TRACE_GYRO);
    CHECK_EQ_I(stub_motion_speed, 4000);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_TRACE);

    /* ---- 6. 非法模式 / 关闭 ---- */
    CHECK_EQ_I(HostExt_DriveSet(9u, 0, 0), -1);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_TRACE);   /* 状态没被破坏 */
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_OFF, 0, 0), 0);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_OFF);
    CHECK_EQ_I(HostExt_DriveEnabled(), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);
    CHECK(stub_sc_stops >= 1);

    /* ---- 7. 链路看门狗：499ms 不动，500ms 停车 ---- */
    reset_all();
    host_tick_set(2000U);
    HostExt_Init();                                 /* s_keepalive = 2000 */
    HostExt_DriveSet(HOST_DRIVE_WHEELS, 100, 100);
    CHECK_EQ_I(HostExt_DriveWatchdogTrips(), 0);

    HostExt_Task(2499U);
    CHECK_EQ_I(HostExt_DriveEnabled(), 1);          /* 499ms：还活着 */
    HostExt_DriveKeepAlive(2500U);                  /* 收到一条合法帧 */
    HostExt_Task(2999U);
    CHECK_EQ_I(HostExt_DriveEnabled(), 1);          /* 又续了 499ms */
    HostExt_Task(3000U);
    CHECK_EQ_I(HostExt_DriveEnabled(), 0);          /* 500ms：停车 */
    CHECK_EQ_I(HostExt_DriveWatchdogTrips(), 1);
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);

    HostExt_Task(9000U);
    CHECK_EQ_I(HostExt_DriveWatchdogTrips(), 1);    /* 已经停了就不再计数 */

    /* ---- 8. 动作表：读、写、夹限、越界 ---- */
    reset_all();
    CHECK_EQ_I(HostExt_ActionSet(3, 100, 20), 1);
    CHECK_EQ_I(HostExt_ActionGet(3, &a, &b), 1);
    CHECK_EQ_I(a, 100);
    CHECK_EQ_I(b, 20);
    CHECK_EQ_I(HostExt_ActionSet(0, 1, 1), 0);
    CHECK_EQ_I(HostExt_ActionSet(SERVO_ACTION_N + 1, 1, 1), 0);
    CHECK_EQ_I(HostExt_ActionGet(0, &a, &b), 0);
    CHECK_EQ_I(HostExt_ActionGet(SERVO_ACTION_N + 1, &a, &b), 0);
    CHECK(HostExt_ActionName(1) != NULL && HostExt_ActionName(1)[0] != '\0');
    CHECK(HostExt_ActionName(SERVO_ACTION_N + 1)[0] == '\0');

    CHECK_EQ_I(HostExt_ActionPlay(3), 1);
    CHECK_EQ_I(stub_servo_apply_n, 3);
    CHECK_EQ_I(HostExt_CurrentAction(), 3);
    CHECK_EQ_I(HostExt_ActionPlay(0), 0);
    CHECK_EQ_I(HostExt_CurrentAction(), 3);         /* 失败不改状态 */
    CHECK_EQ_I(HostExt_ActionPlay(SERVO_ACTION_N + 1), 0);

    /* ---- 9. 舵机：夹限 + 中位偏差立刻生效 ---- */
    reset_all();
    CHECK_EQ_I(HostExt_ServoSet(1, 90), 1);
    CHECK_EQ_I(stub_servo_ctl[1], 90);
    CHECK_EQ_I(HostExt_ServoSet(1, 200), 1);
    CHECK_EQ_I(stub_servo_ctl[1], 180);             /* 夹到 180 */
    CHECK_EQ_I(HostExt_ServoSet(1, -5), 1);
    CHECK_EQ_I(stub_servo_ctl[1], 0);               /* 夹到 0 */
    CHECK_EQ_I(HostExt_ServoSet(3, 90), 0);
    CHECK_EQ_I(HostExt_ServoDeviation(5, 10), 0);

    CHECK_EQ_I(HostExt_ServoSet(2, 60), 1);
    CHECK_EQ_I(HostExt_ServoDeviation(2, 500), 1);  /* 替身不自己夹，只记录 */
    CHECK_EQ_I(stub_servo_dev[2], 500);
    CHECK(stub_servo_angle[2] == 60.0f);            /* 改完偏差立刻按当前角度重发 */
    CHECK(HostExt_ServoAngle(2) == 60.0f);

    CHECK_EQ_I(HostExt_ServoCenter(), 1);
    CHECK_EQ_I(stub_servo_ctl[1], 0);
    CHECK_EQ_I(stub_servo_ctl[2], 0);

    /* ---- 10. 固化：两条路都落到 nvram_touch() ---- */
    reset_all();
    CHECK_EQ_I(HostExt_SaveParams(), 1);
    CHECK_EQ_I(stub_nvram_touch, 1);
    CHECK_EQ_I(HostExt_ActionSave(), 1);
    CHECK_EQ_I(stub_nvram_touch, 2);
    stub_nvram_state = 2u;
    CHECK_EQ_I(HostExt_NvramState(), 2);

    /* ---- 11. 遥测只读量（TELEM2 用） ---- */
    reset_all();
    {
        uint8_t gray[8];
        HostExt_GrayLevel(gray);
        CHECK_EQ_I(gray[0], 5);
        CHECK_EQ_I(gray[7], 75);
    }
    CHECK(HostExt_TracePos() == 4.25f);
    CHECK(HostExt_TracePid() == -123.0f);
    CHECK(HostExt_AngleErr() == 1.25f);
    CHECK(HostExt_AngleRateRef() == 2.5f);
    CHECK(HostExt_AngleCorr() == -3.75f);
    CHECK_EQ_I(HostExt_TraceCross(), 1);
    stub_motion_mode = 3;                           /* 替身里的 Motion_GetMode() */
    CHECK_EQ_I(HostExt_MotionMode(), 3);
    CHECK_EQ_I(HostExt_MotionFault(), 0);

    /* ---- 12. 停车必须真停（用户 m29239：stop 之后车还在跑，只有拔电池才停） ----
     * WHEELS 直给模式下 Motion 一直停在 IDLE，而 motion.c 对同模式是早退 ⇒
     * 原来 HostExt_DriveStop() 里的 Motion_SetMode(MOTION_IDLE) 等于空操作，
     * 速度环照旧使能。现在它必须显式 SpeedControl_Stop()。 */
    reset_all();
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_WHEELS, 200, 200), 0);
    CHECK_EQ_I(stub_sc_enabled, 1);
    {
        int stops_before = stub_sc_stops;

        HostExt_DriveStop();
        CHECK(stub_sc_stops > stops_before);        /* 真调了 Stop（它会 Load(0,0)） */
        CHECK_EQ_I(stub_sc_enabled, 0);             /* 使能清了 = 电机不再带电 */
        CHECK_EQ_I(HostExt_MotorEnabled(), 0);
        CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);
        CHECK_EQ_I(HostExt_DriveEnabled(), 0);
        CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_OFF);
    }

    /* ---- 13. 母线低压保护：拔电池 = 停机，再插上也不自己跑（用户 m29239） ----
     * 拔电机电池时 MCU 还活着（USB 供电），母线实测 0.9V —— 非 0，所以
     * "I2C 读失败记 0" 与"电池被拔"必须区分开，否则一次总线抖动就掐了跑着的车。 */
    reset_all();
    host_tick_set(10000U);
    HostExt_Init();
    stubx_taskrun_stops = 0u;
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);

    /* 造一个"正在跑"的现场：驾驶会话 + 任务解释器都活着 */
    HostExt_DriveKeepAlive(10000U);
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_WHEELS, 300, 300), 0);
    stubx_taskrun_running = 1u;

    /* (a) 正常电压（12V 包实测 ~11.8V）：什么都不做 */
    host_tick_set(10100U);
    HostExt_DriveKeepAlive(10100U);
    HostExt_NoteVbusMv(11800U);
    HostExt_Task(10100U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);
    CHECK_EQ_I(stub_sc_enabled, 1);
    CHECK_EQ_I(stubx_taskrun_stops, 0);

    /* (b) I2C 读失败（host_link 记 0）：绝不当成断电 */
    host_tick_set(10200U);
    HostExt_DriveKeepAlive(10200U);
    HostExt_NoteVbusMv(0U);
    HostExt_Task(10200U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);
    CHECK_EQ_I(stub_sc_enabled, 1);

    /* (c) 读数过期（>HOST_VBUS_STALE_MS 没更新）：也不动手 */
    host_tick_set(11400U);
    HostExt_DriveKeepAlive(11400U);
    HostExt_Task(11400U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);
    CHECK_EQ_I(stub_sc_enabled, 1);

    /* (d) 拔电池：母线 0.9V，连续 HOST_VBUS_LOW_MS(500ms) 才动手 */
    host_tick_set(12100U);
    HostExt_DriveKeepAlive(12100U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(12100U);                           /* 第一次低压：开始计时 */
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);

    host_tick_set(12599U);
    HostExt_DriveKeepAlive(12599U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(12599U);                           /* 499ms：还不到 */
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 0);
    CHECK_EQ_I(stub_sc_enabled, 1);

    host_tick_set(12600U);
    HostExt_DriveKeepAlive(12600U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(12600U);                           /* 500ms：动手 */
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 1);
    CHECK_EQ_I(stub_sc_enabled, 0);                 /* 电机不再带电 */
    CHECK_EQ_I(stub_motion_mode, MOTION_IDLE);
    CHECK_EQ_I(stubx_taskrun_stops, 1);             /* 任务也停了 */
    CHECK_EQ_I(stubx_taskrun_running, 0u);
    CHECK_EQ_I(HostExt_DriveEnabled(), 0);          /* 驾驶会话清掉 */

    /* (e) 同一轮低压不重复动手 */
    host_tick_set(13100U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(13100U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 1);
    CHECK_EQ_I(stubx_taskrun_stops, 1);

    /* (f) 电压回来：只复位监测，不会自己起步 */
    host_tick_set(13200U);
    HostExt_NoteVbusMv(11800U);
    HostExt_Task(13200U);
    CHECK_EQ_I(stub_sc_enabled, 0);
    CHECK_EQ_I(HostExt_DriveEnabled(), 0);

    /* (g) 再拔一次 = 新的一轮，又动手一次 */
    host_tick_set(13300U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(13300U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 1);
    host_tick_set(13800U);
    HostExt_NoteVbusMv(900U);
    HostExt_Task(13800U);
    CHECK_EQ_I(HostExt_LowVoltageDisarms(), 2);
    CHECK_EQ_I(stubx_taskrun_stops, 2);

    return T_SUMMARY();
}
