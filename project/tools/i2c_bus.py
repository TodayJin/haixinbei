"""I2C1 总线校验：上拉阻值 / 总线电容 / 上升时间 / 空闲电平。

板上 R4/R5 = 4.7k -> 3.3V；灰度（循迹）模块自带 10k 并联。
模型：tr = ln(0.7/0.3) * Rp * Cb = 0.847 * Rp * Cb
限值：100kHz tr<=1000ns，400kHz tr<=300ns，低电平灌电流 <=3mA，Rp_min=(3.3-0.4)/3mA。
"""
import math

K = math.log(0.7 / 0.3)  # 0.847
VDD = 3.3
VOL = 0.4
IOL_MAX = 3e-3


def par(a, b):
    return a * b / (a + b)


def report(name, rp):
    print(f"--- {name}: Rp = {rp:.0f} ohm ---")
    for C in (50, 100, 150, 200, 250, 370):
        tr = K * rp * C * 1e-12
        print("  Cb=%3d pF -> tr = %7.1f ns" % (C, tr * 1e9))
    print("  100kHz 允许最大 Cb = %.0f pF" % (1000e-9 / (K * rp) * 1e12))
    print("  400kHz 允许最大 Cb = %.0f pF" % (300e-9 / (K * rp) * 1e12))
    print("  低电平灌电流 = %.2f mA (限 %.0f mA)" % ((VDD - VOL) / rp * 1e3, IOL_MAX * 1e3))


Rp_board, Rp_mod = 4700.0, 10000.0
report("板上 4.7k 单独", Rp_board)
report("板上 4.7k 并联模块 10k", par(Rp_board, Rp_mod))
print("Rp_min = (3.3-0.4)/3mA = %.0f ohm" % ((VDD - VOL) / IOL_MAX))

# 若模块那 10k 接到 5V，空闲电平会被两只上拉分压抬高
print("模块 10k 接 5V 时总线空闲电压 = %.2f V (>=3.6V 需拆掉模块上拉)"
      % ((VDD / Rp_board + 5.0 / Rp_mod) / (1 / Rp_board + 1 / Rp_mod)))
