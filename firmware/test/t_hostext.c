/**
 * @file  t_hostext.c
 * @brief host_ext.c（上位机扩展功能胶水层）的主机测试
 *
 * 链接真 `App/Svc/host_ext.c` + `test/stub/host_appstub.c`（控制模块替身）。
 * 钉死的是**调用序列与安全策略**：
 *   - 驾驶三种模式各自该动哪些模块、顺序对不对；
 *   - 任务(renwu)在跑时一律拒绝；
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
extern int   stub_servo_apply_n;
extern int   stub_servo_ctl[3];
extern int   stub_servo_dev[3];
extern float stub_servo_angle[3];
extern int   stub_nvram_touch;
extern uint8_t stub_nvram_state;
extern float Yaw, Pitch, Roll;

/* host_board.c 里定义的真变量 */
extern uint8_t renwu_flag;
extern uint8_t black_num;

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
    stub_servo_apply_n = 0;
    stub_servo_ctl[1] = stub_servo_ctl[2] = 0;
    stub_servo_dev[1] = stub_servo_dev[2] = 0;
    stub_servo_angle[1] = stub_servo_angle[2] = 0.0f;
    stub_nvram_touch = 0;
    stub_nvram_state = 0;
    Yaw = Pitch = Roll = 0.0f;
    renwu_flag = 0u;
    black_num = 0u;
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

    /* ---- 2. 任务在跑时拒绝远程驾驶 ---- */
    reset_all();
    renwu_flag = 1u;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_WHEELS, 300, 300), -2);
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_OFF);
    CHECK_EQ_I(stub_sc_enabled, 0);          /* 一根手指头都没动过 */
    CHECK_EQ_I(stub_motion_mode, 0);
    renwu_flag = 0u;

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

    /* ---- 4. BASE：角度环 + 速度环；基准航向只在"刚进模式"时锁存 ---- */
    reset_all();
    Yaw = 10.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 5000, 30), 0);
    CHECK_EQ_I(stub_motion_mode, MOTION_GYRO);
    CHECK_EQ_I(stub_motion_speed, 5000);
    CHECK(stub_gyro_target == 40.0f);              /* 10 + 30 */
    CHECK_EQ_I(HostExt_DriveMode(), HOST_DRIVE_BASE);

    Yaw = 50.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 4000, 10), 0);
    CHECK(stub_gyro_target == 20.0f);              /* 基准仍是 10（50 只是当前航向） */
    CHECK_EQ_I(stub_motion_speed, 4000);

    /* 转个接近 ±180 的目标，确认走 NormalizeAngle 折回 */
    reset_all();
    Yaw = 170.0f;
    CHECK_EQ_I(HostExt_DriveSet(HOST_DRIVE_BASE, 3000, 40), 0);
    CHECK(stub_gyro_target == -150.0f);            /* 170 + 40 = 210 ⇒ −150 */

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
    black_num = 7u;
    CHECK_EQ_I(HostExt_BlackNum(), 7);
    CHECK_EQ_I(HostExt_RenwuFlag(), 0);
    stub_motion_mode = 3;                           /* 替身里的 Motion_GetMode() */
    CHECK_EQ_I(HostExt_MotionMode(), 3);
    CHECK_EQ_I(HostExt_MotionFault(), 0);

    return T_SUMMARY();
}
