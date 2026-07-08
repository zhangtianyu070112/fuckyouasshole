"""
Text variant templates for training data generation.

Each alert/system rule has 6 Chinese text variants with {placeholder} slots.
Placeholders are filled with actual flight parameter values at generation time.

Template variables available:
  {agl}        — AGL altitude (ft)
  {alt}        — MSL altitude (ft)
  {vs}         — vertical speed (fpm, absolute value used where appropriate)
  {ias}        — indicated airspeed (kts)
  {gs}         — ground speed (kts)
  {mach}       — Mach number
  {roll}       — roll angle (deg)
  {pitch}      — pitch angle (deg)
  {hdg}        — heading (deg)
  {n1_0}, {n1_1} — N1 engine left/right (%)
  {egt_0}, {egt_1} — EGT engine left/right (°C)
  {egt_max}    — max EGT (°C)
  {flap}       — flap ratio (0..1)
  {gear}       — gear deployed (0/1)
  {fuel}       — total fuel (lbs)
  {cdi}        — CDI deviation (dots)
  {delta}      — IAS delta for windshear (kts)
  {phase}      — flight phase name (Chinese)
  {oat}        — outside air temperature (°C)
  {cabin_alt}  — cabin altitude (ft)
  {oil_press}  — min oil pressure (psi)
  {n1_diff}    — N1 difference between engines (%)
  {fuel_imb}   — fuel imbalance (lbs)
  {bus_volts}  — electrical bus voltage
  {apu_egt}    — APU EGT (°C)
  {hours}      — estimated endurance (hours)
  {fl}         — flight level
"""

# =============================================================================
# GPWS Alert Templates (13 types × 6 variants = 78 templates)
# =============================================================================

GPWS_TEMPLATES = {
    "PULL_UP": [
        "PULL UP！无线电高度{agl:.0f}英尺，下降率{vs:.0f}英尺/分钟，立即拉起！",
        "警告！近地！高度{agl:.0f}英尺，下降率{vs:.0f}英尺/分钟，执行改出程序！",
        "拉起！拉起！地形接近！高度{agl:.0f}英尺！推大油门带杆拉起！",
        "PULL UP！{agl:.0f}英尺！下降率{vs:.0f}英尺/分钟！最大推力复飞！",
        "紧急拉起！{agl:.0f}英尺地形迫近！垂直速度{vs:.0f}英尺/分钟！",
        "PULL UP! PULL UP! AGL {agl:.0f}ft, sink {vs:.0f}fpm, ROTATE NOW!",
    ],

    "WINDSHEAR": [
        "风切变！空速突变{delta:.0f}节！{agl:.0f}英尺！最大推力保持姿态！",
        "WINDSHEAR! WINDSHEAR! IAS change {delta:.0f}kts at {agl:.0f}ft!",
        "风切变警告！空速变化{delta:.0f}节！设置TOGA推力！保持俯仰姿态！",
        "风切变！{delta:.0f}节速度突变！{agl:.0f}英尺！执行风切变改出程序！",
        "WINDSHEAR! {delta:.0f}kts! {agl:.0f}ft! TOGA THRUST, PITCH 15°!",
        "风切变探测！空速变化{delta:.0f}节，{agl:.0f}英尺，立即执行改出！",
    ],

    "MASTER_WARNING": [
        "主警告！检查EICAS告警信息！立即确认并执行相应检查单！",
        "MASTER WARNING! 检查所有系统告警！确认故障源！",
        "主警告灯亮！检查发动机/燃油/液压/电源系统状态！",
        "WARNING! MASTER WARNING ACTIVE! CHECK SYSTEMS IMMEDIATELY!",
        "注意！主警告触发！扫描仪表确认故障，执行非正常检查单！",
        "主警告！立即检查各系统参数，确认故障后执行QRH程序！",
    ],

    "MASTER_CAUTION": [
        "主警戒！检查系统异常指示，确认并评估是否需要机组响应！",
        "MASTER CAUTION! 检查EICAS异常的琥珀色参数！",
        "主警戒灯亮！某项系统参数偏离正常范围，核实具体故障！",
        "CAUTION! MASTER CAUTION ACTIVE! CHECK AMBER ALERTS!",
        "注意：主警戒触发了！检查各系统有无琥珀色异常显示！",
        "主警戒触发，扫描仪表确认非正常参数，按需执行检查单！",
    ],

    "TERRAIN": [
        "地形警告！{agl:.0f}英尺！下降率{vs:.0f}英尺/分钟！立即减小下降率！",
        "TERRAIN! TERRAIN! {agl:.0f}ft, {vs:.0f}fpm descent! CORRECT!",
        "地形迫近！高度{agl:.0f}英尺，下沉率{vs:.0f}英尺/分钟！注意地形！",
        "注意地形！{agl:.0f}英尺！下降率过大！{vs:.0f}英尺/分钟！立即改平！",
        "TERRAIN AHEAD! {agl:.0f}ft AGL, sink {vs:.0f}fpm! PULL UP!",
        "地形警告：{agl:.0f}英尺，{vs:.0f}英尺/分钟下降中，立即减小下降率！",
    ],

    "SINK_RATE": [
        "注意：下降率{vs:.0f}英尺/分钟过大！当前高度{agl:.0f}英尺，减小下降率至1000以下！",
        "SINK RATE！垂直速度{vs:.0f}英尺/分钟，{agl:.0f}英尺高度，带杆减小下降率！",
        "下沉率过大！{vs:.0f}英尺/分钟，高度{agl:.0f}英尺，柔和带杆控制下降率！",
        "下降率警告！{vs:.0f}fpm，{agl:.0f}英尺，立即减小至1500fpm以下！",
        "SINK RATE {vs:.0f}fpm! {agl:.0f}ft AGL! REDUCE SINK RATE!",
        "注意下沉！当前{agl:.0f}英尺，下降率{vs:.0f}fpm，柔和带杆修正！",
    ],

    "TOO_LOW_GEAR": [
        "起落架未放下！高度低于{agl:.0f}英尺！立即放下起落架！",
        "TOO LOW GEAR！{agl:.0f}英尺！起落架手柄放下！执行复飞或放轮！",
        "警告：起落架未放下！当前高度{agl:.0f}英尺，执行复飞或立即放轮！",
        "GEAR NOT DOWN! {agl:.0f}ft! LOWER LANDING GEAR NOW!",
        "起落架！起落架！高度{agl:.0f}英尺，起落架手柄未放下！",
        "高度{agl:.0f}英尺，起落架未放出，立即检查并放下起落架！",
    ],

    "TOO_LOW_FLAPS": [
        "襟翼未放置着陆构型！高度{agl:.0f}英尺！当前襟翼{flap:.0%}，需至少25%！",
        "TOO LOW FLAPS！{agl:.0f}英尺襟翼仅{flap:.0%}！放襟翼至着陆位！",
        "警告：着陆襟翼未设置！{agl:.0f}英尺襟翼{flap:.0%}，立即放襟翼！",
        "FLAPS NOT SET! {agl:.0f}ft, flaps {flap:.0%}, SET FLAPS 30 OR 40!",
        "襟翼警告！高度{agl:.0f}英尺，襟翼{flap:.0%}不满足着陆要求！",
        "进近构型警告：{agl:.0f}英尺，襟翼仅{flap:.0%}，需放至襟翼30或40！",
    ],

    "GLIDESLOPE": [
        "下滑道偏离！CDI偏差{cdi:.1f}个点，{agl:.0f}英尺，修正下滑道！",
        "GLIDESLOPE！偏差{cdi:.1f}点！{agl:.0f}英尺！复飞或修正航迹！",
        "下滑道警告！偏离{cdi:.1f}个点，高度{agl:.0f}英尺，修正下滑航迹！",
        "GLIDESLOPE DEVIATION {cdi:.1f} dots at {agl:.0f}ft! CORRECT!",
        "注意下滑道！当前偏差{cdi:.1f}点，{agl:.0f}英尺，立即修正！",
        "下滑道偏离{cdi:.1f}点！{agl:.0f}英尺！检查ILS进近参数并修正！",
    ],

    "BANK_ANGLE": [
        "坡度警告！当前坡度{roll:.0f}度超过35度限制！立即改平坡度！",
        "BANK ANGLE！坡度{roll:.0f}度！向下压杆减小坡度至30度以下！",
        "坡度{roll:.0f}度过大！高度仅{agl:.0f}英尺，向反方向压杆改平！",
        "坡度超限！坡度{roll:.0f}度，检查飞行指引，反方向压杆！",
        "坡度角{roll:.0f}度！收杆改平！保持机翼水平！",
        "CAUTION! BANK ANGLE {roll:.0f}° at {agl:.0f}ft AGL! LEVEL WINGS!",
    ],

    "OVERSPEED": [
        "超速警告！空速{ias:.0f}节超过340节限制！收油门减速！",
        "OVERSPEED！{ias:.0f}节！立即减速至340节以下！收油门！",
        "速度过快！{ias:.0f}节超限！收油门，必要时放出减速板！",
        "OVERSPEED {ias:.0f}kts! REDUCE THRUST, EXTEND SPEEDBRAKE!",
        "超速！{ias:.0f}节！减小推力！减速至巡航速度！",
        "速度警告：{ias:.0f}节超过Vmo限制，收油门减速至340节以下！",
    ],

    "STALL": [
        "失速警告！空速{ias:.0f}节低于110节，推杆减小迎角，增加推力！",
        "失速！失速！空速仅{ias:.0f}节！推杆！推油门至TOGA！",
        "STALL！{ias:.0f}节！立即减小迎角，最大推力改出！",
        "失速警告！空速{ias:.0f}节，收减速板，推油门至最大改出！",
        "飞机失速！{ias:.0f}节，执行失速改出：推杆→推力→改平！",
        "STALL WARNING! {ias:.0f}kts, PUSH NOSE DOWN, MAX THRUST!",
    ],

    "MINIMUMS": [
        "决断高度！{agl:.0f}英尺！",
        "MINIMUMS! {agl:.0f}ft! DECISION HEIGHT!",
        "决断高{agl:.0f}英尺！决断！",
        "MINIMUMS! DECISION HEIGHT REACHED! {agl:.0f}ft!",
        "{agl:.0f}英尺决断高度！继续或复飞！",
        "决断高度！MINIMUMS! {agl:.0f}ft! 决断！",
    ],
}

# =============================================================================
# System Deviation Templates (10 types × 6 variants = 60 templates)
# =============================================================================

SYSTEM_TEMPLATES = {
    "ENG_OVERHEAT": [
        "发动机过热！EGT {egt_max:.0f}°C超过850°C限制！减小推力，监控EGT！",
        "警告：发动机EGT {egt_max:.0f}°C超温！N1 {n1_0:.0f}%/{n1_1:.0f}%，立即减小推力！",
        "ENG OVERHEAT! EGT {egt_max:.0f}°C exceeds 850°C! REDUCE THRUST!",
        "发动机温度过高！{egt_max:.0f}°C！收油门至EGT降至800°C以下！",
        "超温警告：EGT {egt_max:.0f}°C，检查发动机引气及燃油流量！",
        "发动机EGT超限！{egt_max:.0f}°C，减小N1，监控温度趋势！",
    ],

    "ENG_ASYM": [
        "发动机推力不对称！N1偏差{n1_diff:.1f}%！左右N1分别为{n1_0:.0f}%和{n1_1:.0f}%，检查油门！",
        "推力不对称警告！左右发动机N1相差{n1_diff:.1f}%，调整油门平衡推力！",
        "ENG THRUST ASYMMETRY! N1 diff {n1_diff:.1f}%! CHECK THROTTLE POSITIONS!",
        "注意：N1偏差{n1_diff:.1f}%超过5%限制，检查发动机参数，平衡推力！",
        "双发推力不平衡！N1左{n1_0:.0f}%右{n1_1:.0f}%，偏差{n1_diff:.1f}%！",
        "推力不对称！N1差值{n1_diff:.1f}%，检查燃油流量和发动机仪表！",
    ],

    "FUEL_IMBALANCE": [
        "燃油不平衡！左右油箱偏差估计{n1_diff:.1f}%！注意燃油平衡！",
        "注意：燃油不平衡！检查交输供油，平衡左右油箱！",
        "FUEL IMBALANCE! Estimated difference {fuel_imb:.0f}lbs! CROSSFEED!",
        "燃油不平衡警告！左右油箱偏差约{fuel_imb:.0f}磅！执行燃油平衡程序！",
        "燃油分布不均！偏差约{fuel_imb:.0f}磅，打开交输活门平衡！",
        "注意燃油平衡！左右偏差{fuel_imb:.0f}磅，监控燃油温度及消耗！",
    ],

    "OIL_PRESS_LOW": [
        "滑油压力低！最低{oil_press:.0f}psi低于25psi限制！检查发动机状态！",
        "警告：滑油压力{oil_press:.0f}psi异常偏低！监控滑油温度和压力！",
        "OIL PRESSURE LOW! {oil_press:.0f}psi below 25psi! CHECK ENGINE!",
        "滑油压力警告！{oil_press:.0f}psi！可能滑油泄漏或泵故障，监控参数！",
        "发动机滑油压力低！{oil_press:.0f}psi！检查滑油量和滤网！",
        "低滑油压力！{oil_press:.0f}psi低于正常范围！备降最近机场！",
    ],

    "CABIN_ALT_HIGH": [
        "座舱高度过高！{cabin_alt:.0f}英尺超过10000英尺！检查增压系统！",
        "警告：座舱高度{cabin_alt:.0f}英尺超限！旅客氧气面罩可能已脱落！紧急下降！",
        "CABIN ALTITUDE HIGH! {cabin_alt:.0f}ft! EMERGENCY DESCENT TO 10000ft!",
        "座舱增压失效！舱内高度{cabin_alt:.0f}英尺！立即下降至10000英尺以下！",
        "座舱高度警告！{cabin_alt:.0f}英尺！检查增压模式选择器和外流活门！",
        "HIGH CABIN ALT! {cabin_alt:.0f}ft! OXYGEN MASKS DEPLOYED! DESCEND NOW!",
    ],

    "BUS_VOLT_ABNORM": [
        "电源总线电压异常！当前{bus_volts:.1f}V偏离正常28V范围！检查发电机！",
        "电气系统异常！总线电压{bus_volts:.1f}V！检查发电机负载和电瓶状态！",
        "BUS VOLTAGE ABNORMAL! {bus_volts:.1f}V out of 24-32V range! CHECK GENERATORS!",
        "电源警告：总线电压{bus_volts:.1f}V异常！检查发电机控制组件和APU电源！",
        "电气总线电压偏离正常！{bus_volts:.1f}V，检查发电机开关及负载分配！",
        "注意电源！总线电压{bus_volts:.1f}V不在正常范围，核实发电机电门！",
    ],

    "TAKEOFF_CONFIG": [
        "起飞构型警告！襟翼仅{flap:.0%}，起飞至少需要襟翼1（10%）！",
        "TAKEOFF CONFIG! Flaps {flap:.0%} insufficient! SET TAKEOFF FLAPS!",
        "起飞构型不正确！襟翼{flap:.0%}低于起飞最小设定，检查襟翼手柄！",
        "警告：无法起飞！襟翼未设置起飞构型！当前仅{flap:.0%}，需至少10%！",
        "起飞构型警告！襟翼{flap:.0%}不满足起飞要求，设置襟翼1或以上！",
        "CONFIG TAKEOFF! Flaps {flap:.0%}! SET FLAPS FOR TAKEOFF!",
    ],

    "LOW_FUEL": [
        "燃油不足！剩余{fuel:.0f}磅，预计续航{hours:.1f}小时！考虑备降加油！",
        "低燃油警告！仅剩{fuel:.0f}磅燃油！联系ATC申请优先着陆！",
        "LOW FUEL! {fuel:.0f}lbs remaining! ~{hours:.1f}hrs endurance! DIVERT!",
        "燃油量低！{fuel:.0f}磅约{hours:.1f}小时！检查最近备降机场！",
        "注意！燃油不足！{fuel:.0f}磅剩余！实行燃油节约措施！",
        "燃油警告！{fuel:.0f}磅，约{hours:.1f}小时续航！立即评估备降方案！",
    ],

    "ICING_CONDITION": [
        "结冰条件！OAT {oat:.0f}°C低于10°C，机翼防冰未开启！立即接通机翼防冰！",
        "ICING CONDITION! OAT {oat:.0f}°C, wing anti-ice OFF! ACTIVATE ANTI-ICE!",
        "注意结冰！外界温度{oat:.0f}°C，机翼防冰未接通！接通防冰系统！",
        "结冰警告！OAT {oat:.0f}°C存在积冰风险，机翼防冰电门未在ON位！",
        "机翼可能积冰！外界{oat:.0f}°C，防冰未开！立即接通发动机和机翼防冰！",
        "结冰条件满足！OAT {oat:.0f}°C，需开启防冰！检查发动机及机翼防冰电门！",
    ],

    "APU_FIRE": [
        "APU火警！APU EGT {apu_egt:.0f}°C异常！执行APU灭火程序！",
        "APU FIRE! EGT {apu_egt:.0f}°C! APU FIRE HANDLE — PULL AND DISCHARGE!",
        "APU火警警告！EGT {apu_egt:.0f}°C超过760°C！立即执行APU火警检查单！",
        "警告：APU着火！EGT {apu_egt:.0f}°C！拉出APU灭火手柄！释放灭火瓶！",
        "APU火灾！EGT {apu_egt:.0f}°C！关闭APU，拉灭火手柄，释放灭火剂！",
        "APU FIRE detected! EGT {apu_egt:.0f}°C! PULL APU FIRE HANDLE! DISCHARGE BOTTLE!",
    ],

    "OIL_TEMP_HIGH": [
        "滑油温度过高！{oil_temp_0:.0f}°C超过120°C限制！减小推力，监控滑油温度趋势！",
        "OIL TEMP HIGH! {oil_temp_0:.0f}°C exceeds 120°C! REDUCE THRUST AND MONITOR!",
        "发动机滑油温度异常！{oil_temp_0:.0f}°C！检查滑油散热器，考虑减小N1！",
        "滑油超温警告！{oil_temp_0:.0f}°C！核实滑油量和滑油压力，减速降温！",
        "滑油温度{fuel_imb:.0f}°C偏高！检查滑油冷却系统，减小发动机负载！",
        "OIL TEMPERATURE HIGH! {oil_temp_0:.0f}°C above limit! VERIFY OIL QUANTITY AND PRESSURE!",
    ],

    "HYD_PRESS_LOW": [
        "液压压力低！A系统{p0:.0f}psi/B系统{p1:.0f}psi低于2800psi！检查液压泵和系统泄漏！",
        "HYD PRESS LOW! A={p0:.0f} B={p1:.0f}psi below 2800psi! CHECK HYD PUMPS!",
        "液压系统压力不足！{p0:.0f}/{p1:.0f}psi！可能影响飞控和起落架操作！",
        "液压压力警告！A={p0:.0f} B={p1:.0f}psi，低于最低限制！检查液压油箱和EDP！",
        "注意液压！{p0:.0f}/{p1:.0f}psi低压！检查液压泵电门和系统泄漏！",
        "HYDRAULIC PRESSURE LOW! System A={p0:.0f} B={p1:.0f}psi! Check pumps and quantity!",
    ],

    "HYD_QTY_LOW": [
        "液压油量低！{q0:.0%}/{q1:.0%}低于20%！检查液压系统泄漏，考虑备降！",
        "HYD QTY LOW! {q0:.0%}/{q1:.0%} below 20%! CHECK FOR LEAKS! PREPARE FOR DIVERT!",
        "液压油箱油量不足！{q0:.0%}/{q1:.0%}！监控液压压力，如继续下降则宣布紧急状态！",
        "注意液压油量！仅{q0:.0%}/{q1:.0%}剩余！核实管路无泄漏，准备备降！",
        "液压油量不足警告！{q0:.0%}/{q1:.0%}低于最低限制！检查泄漏并考虑备降！",
        "HYD QTY CRITICAL! {q0:.0%}/{q1:.0%} remaining! Possible leak! DIVERT!",
    ],
}

# =============================================================================
# Derived Alert Templates (oil temp, hydraulics — ~4 templates each)
# =============================================================================

DERIVED_ALERT_TEMPLATES = {
    "OIL_TEMP_HIGH": [
        "滑油温度过高！{oil_temp_0:.0f}°C超过120°C限制！减小推力，监控滑油温度趋势！",
        "OIL TEMP HIGH! {oil_temp_0:.0f}°C exceeds 120°C! REDUCE THRUST AND MONITOR!",
        "发动机滑油温度异常！{oil_temp_0:.0f}°C！检查滑油散热器，考虑减小N1！",
        "滑油超温警告！{oil_temp_0:.0f}°C！核实滑油量和滑油压力，减速降温！",
    ],

    "HYD_PRESS_LOW": [
        "液压压力低！A系统{p0:.0f}psi/B系统{p1:.0f}psi低于2800psi！检查液压泵和系统泄漏！",
        "HYD PRESS LOW! A={p0:.0f} B={p1:.0f}psi below 2800psi! CHECK HYD PUMPS!",
        "液压系统压力不足！{p0:.0f}/{p1:.0f}psi！可能影响飞控和起落架操作！",
        "液压压力警告！A={p0:.0f} B={p1:.0f}psi，低于最低限制！检查液压油箱和EDP！",
    ],

    "HYD_QTY_LOW": [
        "液压油量低！{q0:.0%}/{q1:.0%}低于20%！检查液压系统泄漏，考虑备降！",
        "HYD QTY LOW! {q0:.0%}/{q1:.0%} below 20%! CHECK FOR LEAKS! PREPARE FOR DIVERT!",
        "液压油箱油量不足！{q0:.0%}/{q1:.0%}！监控液压压力，如继续下降则宣布紧急状态！",
        "注意液压油量！仅{q0:.0%}/{q1:.0%}剩余！核实管路无泄漏，准备备降！",
    ],
}

# =============================================================================
# DREF Alert Templates (fire, tcas, doors, elec, etc. — ~4 templates each)
# =============================================================================

DREF_ALERT_TEMPLATES = {
    "FIRE_ENG1": [
        "左发火警！执行发动机火警检查单！关断油门，拉出灭火手柄！",
        "ENGINE 1 FIRE! SHUT OFF FUEL! PULL FIRE HANDLE! DISCHARGE BOTTLE!",
        "左发火警探测！立即执行记忆项目：油门慢车→燃油切断→灭火手柄拉出！",
        "FIRE ENG1! 左发火警！执行QRH发动机火警/严重损坏程序！",
    ],

    "FIRE_ENG2": [
        "右发火警！执行发动机火警检查单！关断油门，拉出灭火手柄！",
        "ENGINE 2 FIRE! SHUT OFF FUEL! PULL FIRE HANDLE! DISCHARGE BOTTLE!",
        "右发火警探测！立即执行记忆项目：油门慢车→燃油切断→灭火手柄拉出！",
        "FIRE ENG2! 右发火警！执行QRH发动机火警/严重损坏程序！",
    ],

    "FIRE_APU": [
        "APU火警！执行APU灭火程序！拉出APU灭火手柄，释放灭火瓶！",
        "APU FIRE! PULL APU FIRE HANDLE! DISCHARGE BOTTLE! EVACUATE IF ON GROUND!",
        "APU舱火警！立即关闭APU，拉灭火手柄，释放灭火剂！地面需紧急撤离！",
        "APU FIRE detected! Pull APU fire handle, rotate to discharge!",
    ],

    "FIRE_WHEEL_WELL": [
        "轮舱火警！检查起落架指示，准备紧急着陆！执行轮舱火警检查单！",
        "WHEEL WELL FIRE! PREPARE FOR EMERGENCY LANDING! EXECUTE CHECKLIST!",
        "主轮舱火警探测！放下起落架，限制机动，立即备降最近机场！",
        "轮舱火警！检查刹车温度，放下起落架风冷，宣布紧急状态！",
    ],

    "FIRE_CARGO": [
        "货舱火警！执行货舱火警检查单！释放货舱灭火瓶！备降最近机场！",
        "CARGO FIRE! DISCHARGE CARGO FIRE BOTTLE! DIVERT TO NEAREST AIRPORT!",
        "货舱烟雾探测！立即释放货舱灭火瓶，宣布紧急状态，备降！",
        "CARGO COMPARTMENT FIRE! Discharge extinguisher, declare emergency, land ASAP!",
    ],

    "TCAS_TA": [
        "TCAS TA！交通警戒！注意观察冲突飞机，准备响应RA指令！",
        "TCAS TRAFFIC ADVISORY! Monitor traffic, be prepared for Resolution Advisory!",
        "交通警戒TA！扫描仪表确认冲突飞机方位和相对高度！",
        "TRAFFIC! TRAFFIC! TCAS TA issued! Monitor vertical speed, await RA!",
    ],

    "TCAS_RA": [
        "TCAS RA！决断咨询！立即按RA指令垂直机动！服从TCAS指示！",
        "TCAS RESOLUTION ADVISORY! FOLLOW RA COMMAND IMMEDIATELY! DISREGARD ATC!",
        "RA！RA！执行TCAS决断咨询机动！监控VSI绿色区域，服从RA！",
        "TCAS RA ACTIVE! Climb/Descend as commanded! ATC notified after maneuver!",
    ],

    "STALL_WARNING": [
        "失速警告！立即推杆减小迎角，最大推力改出！检查飞控构型！",
        "STALL WARNING ACTIVE! PUSH NOSE DOWN! MAX THRUST! RECOVER FROM STALL!",
        "飞机进入失速！推杆至地平线以下，TOGA推力，收减速板！",
        "STALL! STALL! Execute stall recovery: pitch down, full thrust, wings level!",
    ],

    "DOOR_OPEN": [
        "舱门未关锁！检查前后登机门和货舱门指示！立即检查座舱增压！",
        "DOOR WARNING! Entry/cargo door not secured! Check cabin pressurization!",
        "舱门警告灯亮！核实所有舱门关闭锁好！可能影响座舱增压！",
        "DOOR OPEN indication! Verify all doors closed and locked before pressurization!",
    ],

    "ELEC_FAULT": [
        "电气系统故障！检查发电机负载和电瓶状态！可能需启动APU供电！",
        "ELECTRICAL FAULT! Check generator load and battery status! Consider APU start!",
        "电源系统异常！核实发电机开关、汇流条连接和电瓶电压！",
        "ELEC FAULT detected! Monitor bus voltage, shed non-essential loads if needed!",
    ],

    "ANTI_ICE_FAULT": [
        "防冰系统故障！检查机翼/发动机防冰电门和探头加温！注意积冰风险！",
        "ANTI-ICE FAULT! Check wing/cowl anti-ice switches! Monitor for ice accumulation!",
        "防冰系统异常！核实防冰电门位置，检查引气压力和电源！",
        "ANTI-ICE SYSTEM FAULT! Possible ice accumulation! Exit icing conditions if able!",
    ],

    "AP_DISENGAGE": [
        "自动驾驶脱开！注意飞机姿态，人工接管操纵！核实AP断开原因！",
        "AUTOPILOT DISENGAGED! Manual flying now! Check AP disconnect cause!",
        "AP脱开警告！立即人工操纵飞机，检查AP警告灯和音响复位！",
        "AP DISENGAGE! Verify aircraft attitude, check trim, consider re-engaging AP!",
    ],

    "AT_DISENGAGE": [
        "自动油门脱开！人工控制油门！核实速度偏离，检查A/T模式！",
        "AUTOTHROTTLE DISENGAGED! Manual throttle control! Check speed deviation!",
        "A/T脱开！注意空速变化，人工调整油门位置！检查A/T电门！",
        "AUTOTHROTTLE OFF! Monitor IAS closely, manually set appropriate thrust!",
    ],
}

# =============================================================================
# Phase Summary Templates (8 phases × 6 variants = 48 templates)
# =============================================================================

PHASE_TEMPLATES = {
    "TAKEOFF": [
        "起飞阶段：V2+10 {ias:.0f}节，N1 {n1_0:.0f}%/{n1_1:.0f}%，襟翼{flap:.0%}，正上升率{vs:.0f}fpm。",
        "正在起飞：{ias:.0f}节加速中，双发N1接近{n1_0:.0f}%，{agl:.0f}英尺，收起落架。",
        "起飞推力设定：N1 {n1_0:.0f}%，空速{ias:.0f}节增速正常，姿态{agl:.0f}英尺上升中。",
        "TAKEOFF: {ias:.0f}KIAS, N1 {n1_0:.0f}%, flaps {flap:.0%}, climbing through {agl:.0f}ft.",
        "起飞滑跑完成：{agl:.0f}英尺离地，空速{ias:.0f}节，正上升率，收轮。",
        "起飞阶段正常：{ias:.0f}节增速，{agl:.0f}英尺上升率{vs:.0f}fpm，收襟翼中。",
    ],

    "CLIMB1": [
        "初始爬升：通过{agl:.0f}英尺，N1 {n1_0:.0f}%，IAS {ias:.0f}节，襟翼{flap:.0%}收上中。",
        "爬升阶段：高度{alt:.0f}英尺，{ias:.0f}节，上升率{vs:.0f}fpm，双发正常。",
        "初始爬升中：{alt:.0f}英尺，IAS {ias:.0f}节加速至210节，N1 {n1_0:.0f}%。",
        "CLIMB1: Passing {alt:.0f}ft, {ias:.0f}KIAS, {vs:.0f}fpm, flaps retracting.",
        "爬升正常：{alt:.0f}英尺，空速{ias:.0f}节，上升率{vs:.0f}fpm，发动机参数正常。",
        "上升至{alt:.0f}英尺：IAS {ias:.0f}节，N1 {n1_0:.0f}%/{n1_1:.0f}%，EGT {egt_0:.0f}°C。",
    ],

    "CLIMB2": [
        "巡航爬升：{alt:.0f}英尺→目标FL{fl:.0f}，{ias:.0f}节，上升率{vs:.0f}fpm，N1 {n1_0:.0f}%。",
        "爬升至巡航高度：当前{alt:.0f}英尺，IAS {ias:.0f}节，预计到达FL{fl:.0f}，双发正常。",
        "高空爬升中：{alt:.0f}英尺，{ias:.0f}节，上升率{vs:.0f}fpm，OAT {oat:.0f}°C。",
        "CLIMB2: {alt:.0f}ft climbing to FL{fl:.0f}, {ias:.0f}KIAS, {vs:.0f}fpm.",
        "继续爬升至FL{fl:.0f}：{alt:.0f}英尺，N1 {n1_0:.0f}%/{n1_1:.0f}%，马赫{mach:.2f}。",
        "高空爬升：{alt:.0f}英尺→FL{fl:.0f}，马赫{mach:.2f}，燃油{fuel:.0f}磅，一切正常。",
    ],

    "CRUISE": [
        "巡航中：FL{fl:.0f}，马赫{mach:.2f}，航向{hdg:.0f}°，双发正常，燃油{fuel:.0f}磅。",
        "正常巡航：{alt:.0f}英尺，{ias:.0f}节/M{mach:.2f}，N1 {n1_0:.0f}%/{n1_1:.0f}%，OAT {oat:.0f}°C。",
        "巡航状态：FL{fl:.0f}，地速{gs:.0f}节，燃油{fuel:.0f}磅，预计续航{hours:.1f}小时，一切正常。",
        "CRUISE: FL{fl:.0f}, M{mach:.2f}, {ias:.0f}KIAS, fuel {fuel:.0f}lbs, NORMAL.",
        "平稳巡航：{alt:.0f}英尺，航向{hdg:.0f}°，自动驾驶接通，双发EGT {egt_0:.0f}°C/{egt_1:.0f}°C。",
        "巡航阶段：FL{fl:.0f}，IAS {ias:.0f}节，N1 {n1_0:.0f}%/ {n1_1:.0f}%，燃油{fuel:.0f}磅，状态正常。",
    ],

    "DESCENT": [
        "下降阶段：{alt:.0f}英尺→目标高度，{ias:.0f}节，下降率{vs:.0f}fpm，N1 {n1_0:.0f}%。",
        "开始下降：当前{alt:.0f}英尺，{ias:.0f}节，下降率{vs:.0f}fpm，预计{phase}。",
        "下降中：{alt:.0f}英尺，IAS {ias:.0f}节减速至260节，下降率{vs:.0f}fpm，减速板{spdbrk}。",
        "DESCENT: {alt:.0f}ft, {ias:.0f}KIAS, {vs:.0f}fpm, N1 {n1_0:.0f}%, normal.",
        "巡航下降：{alt:.0f}英尺，{ias:.0f}节，{vs:.0f}fpm，预计进近，双发正常。",
        "下降阶段正常：{alt:.0f}英尺，{ias:.0f}节，下降率{vs:.0f}fpm，OAT {oat:.0f}°C。",
    ],

    "APPROACH": [
        "进近阶段：{agl:.0f}英尺AGL，{ias:.0f}节，襟翼{flap:.0%}，起落架{gear_status}，下降率{vs:.0f}fpm。",
        "进近中：{agl:.0f}英尺，IAS {ias:.0f}节，襟翼{flap:.0%}放着陆构型中。",
        "最终进近：{agl:.0f}英尺，空速{ias:.0f}节，{vs:.0f}fpm下滑道跟踪中，CDI {cdi:.1f}点。",
        "APPROACH: {agl:.0f}ft AGL, {ias:.0f}KIAS, flaps {flap:.0%}, gear {gear_status}, {vs:.0f}fpm.",
        "进近构型：{agl:.0f}英尺，{ias:.0f}节，襟翼{flap:.0%}，下滑道跟踪正常。",
        "五边进近：{agl:.0f}英尺AGL，空速{ias:.0f}节，下降率{vs:.0f}fpm，稳定进近中。",
    ],

    "LANDING": [
        "着陆阶段：{agl:.0f}英尺，{ias:.0f}节，襟翼{flap:.0%}，起落架放下，拉平中。",
        "拉平着陆：{agl:.0f}英尺，地速{gs:.0f}节，减速板预位，反推待命。",
        "着陆中：{ias:.0f}节，{agl:.0f}英尺，减速板{spdbrk}，反推{reverser}。",
        "LANDING: {agl:.0f}ft, {ias:.0f}KIAS, flaps {flap:.0%}, gear down, flare.",
        "落地：{ias:.0f}节接地区，减速板放出，反推开锁，自动刹车工作。",
        "着陆滑跑：地速{gs:.0f}节减速中，反推最大，刹车温度正常。",
    ],

    "TAXI": [
        "滑行阶段：地速{gs:.0f}节，APU运转，襟翼收上中，一切正常。",
        "地面滑行：{gs:.0f}节，APU N1 {apu_n1:.0f}%，EGT {apu_egt:.0f}°C，准备关车。",
        "TAXI: {gs:.0f}kts ground speed, APU running, flaps retracting, normal.",
        "着陆后滑行：{gs:.0f}节减速，APU供电，发动机N1 {n1_0:.0f}%慢车。",
        "滑行至停机位：{gs:.0f}节，襟翼{flap:.0%}收起，APU正常运转。",
        "地面滑行正常：速度{gs:.0f}节，APU运转正常，双发慢车参数正常。",
    ],
}

# =============================================================================
# Normal Cruise Check-in Templates (6 variants)
# =============================================================================

NORMAL_TEMPLATES = [
    "巡航中：FL{fl:.0f}，马赫{mach:.2f}，航向{hdg:.0f}°，双发正常，燃油{fuel:.0f}磅，一切正常。",
    "当前{phase}：{alt:.0f}英尺，IAS {ias:.0f}节，N1各{n1_0:.0f}%/{n1_1:.0f}%，状态正常。",
    "飞行状态正常：{phase}，{alt:.0f}英尺，{ias:.0f}节，双发EGT {egt_0:.0f}°C/{egt_1:.0f}°C。",
    "正常巡航：FL{fl:.0f}，马赫{mach:.2f}，燃油{fuel:.0f}磅，预计续航{hours:.1f}小时，一切正常。",
    "一切正常：{phase}，地速{gs:.0f}节，OAT {oat:.0f}°C，航向{hdg:.0f}°，自动驾驶接通。",
    "NORMAL: {phase}, FL{fl:.0f}, M{mach:.2f}, {ias:.0f}KIAS, FUEL {fuel:.0f}LBS, ALL GREEN.",
]

# =============================================================================
# Combined Alert Templates (multi-condition scenarios)
# =============================================================================

# Special templates for when multiple alerts fire simultaneously
COMBINED_TEMPLATES = [
    # SINK_RATE + TOO_LOW_GEAR
    "紧急！下降率{vs:.0f}fpm过大，{agl:.0f}英尺起落架未放下！立即减小下降率并放下起落架！",
    "双重警告：SINK RATE {vs:.0f}fpm + TOO LOW GEAR {agl:.0f}ft！复飞或立即修正！",
    # TERRAIN + SINK_RATE
    "地形警告+下沉率过大！{agl:.0f}英尺，{vs:.0f}fpm下降！立即拉起！",
    "TERRAIN + SINK RATE! {agl:.0f}ft AGL, {vs:.0f}fpm! PULL UP AND REDUCE SINK!",
    # TOO_LOW_GEAR + TOO_LOW_FLAPS
    "着陆构型不完整！{agl:.0f}英尺起落架未放下且襟翼仅{flap:.0%}！执行复飞！",
    "双重构型警告：{agl:.0f}英尺，起落架未放+襟翼{flap:.0%}不足！GO AROUND!",
    # GLIDESLOPE + SINK_RATE
    "下滑道偏离{cdi:.1f}点且下沉率{vs:.0f}fpm过大！{agl:.0f}英尺，修正下滑道！",
    "GLIDESLOPE {cdi:.1f} + SINK RATE {vs:.0f}fpm at {agl:.0f}ft! CORRECT GLIDEPATH!",
    # STALL + PULL_UP
    "双重紧急！失速{ias:.0f}节+近地{agl:.0f}英尺！推杆改出失速后立即拉起！",
    "STALL {ias:.0f}kts + PULL UP {agl:.0f}ft! PUSH→RECOVER→PULL UP! EMERGENCY!",
    # BANK_ANGLE + TERRAIN
    "坡度{roll:.0f}°过大且地形迫近{agl:.0f}英尺！立即改平坡度并拉起！",
    "BANK {roll:.0f}° + TERRAIN {agl:.0f}ft! LEVEL WINGS AND PULL UP!",
]

# =============================================================================
# System prompt (instruction for the model)
# =============================================================================

SYSTEM_PROMPT = (
    "你是B737-800驾驶舱AI副驾驶。监控飞行参数，用中文提供简洁的操作建议或态势说明。"
    "输出不超过50字。一切正常时报告「正常」，异常时指出具体问题和建议操作。"
    "你提供的建议仅供参考，不可替代SOP和GPWS硬告警。"
)

# =============================================================================
# Alert → Category mapping
# =============================================================================

ALERT_CATEGORY_MAP = {
    # GPWS alerts
    "PULL_UP": "A_GPWS",
    "WINDSHEAR": "A_GPWS",
    "MASTER_WARNING": "A_GPWS",
    "MASTER_CAUTION": "A_GPWS",
    "TERRAIN": "A_GPWS",
    "SINK_RATE": "A_GPWS",
    "TOO_LOW_GEAR": "A_GPWS",
    "TOO_LOW_FLAPS": "A_GPWS",
    "GLIDESLOPE": "A_GPWS",
    "BANK_ANGLE": "A_GPWS",
    "OVERSPEED": "A_GPWS",
    "STALL": "A_GPWS",
    "MINIMUMS": "A_GPWS",
    # System deviations
    "ENG_OVERHEAT": "C_SYSTEM",
    "ENG_ASYM": "C_SYSTEM",
    "FUEL_IMBALANCE": "C_SYSTEM",
    "OIL_PRESS_LOW": "C_SYSTEM",
    "CABIN_ALT_HIGH": "C_SYSTEM",
    "BUS_VOLT_ABNORM": "C_SYSTEM",
    "TAKEOFF_CONFIG": "C_SYSTEM",
    "LOW_FUEL": "C_SYSTEM",
    "ICING_CONDITION": "C_SYSTEM",
    "APU_FIRE": "C_SYSTEM",
    "OIL_TEMP_HIGH": "C_SYSTEM",
    "HYD_PRESS_LOW": "C_SYSTEM",
    "HYD_QTY_LOW": "C_SYSTEM",
    # DREF-only alerts (new category)
    "FIRE_ENG1": "F_DREF",
    "FIRE_ENG2": "F_DREF",
    "FIRE_APU": "F_DREF",
    "FIRE_WHEEL_WELL": "F_DREF",
    "FIRE_CARGO": "F_DREF",
    "TCAS_TA": "F_DREF",
    "TCAS_RA": "F_DREF",
    "STALL_WARNING": "F_DREF",
    "DOOR_OPEN": "F_DREF",
    "ELEC_FAULT": "F_DREF",
    "ANTI_ICE_FAULT": "F_DREF",
    "AP_DISENGAGE": "F_DREF",
    "AT_DISENGAGE": "F_DREF",
}

# =============================================================================
# Extra templates — added to increase variety (3+ extra per type)
# Prevents memorization when generating 1000+ samples per alert type.
# =============================================================================

EXTRA_GPWS_TEMPLATES = {
    "PULL_UP": [
        "地形！地形！{agl:.0f}英尺！立即拉起机头至15度仰角！最大推力！",
        "PULL UP! {agl:.0f}ft AGL, sink {vs:.0f}fpm! MAX THRUST, ROTATE NOW!",
        "近地警告！{agl:.0f}英尺，{vs:.0f}fpm下沉！执行地形避让机动！",
    ],
    "WINDSHEAR": [
        "遭遇风切变！IAS波动{delta:.0f}节！保持当前构型，最大推力！",
        "WINDSHEAR AHEAD! {delta:.0f}kts IAS change at {agl:.0f}ft! GO AROUND!",
        "风切变！{agl:.0f}英尺空速骤变{delta:.0f}节！不可收襟翼/起落架！",
    ],
    "MASTER_WARNING": [
        "主警告触发！立即扫描EICAS确认故障源！执行相应非正常检查单！",
        "WARNING! MASTER WARNING! CHECK ALL SYSTEMS AND EXECUTE QRH!",
        "红色主警告！快速检查发动机指示、燃油、液压和电源参数！",
    ],
    "MASTER_CAUTION": [
        "琥珀色主警戒！核实EICAS上异常参数指示，评估运行影响！",
        "CAUTION ACTIVE! REVIEW ABNORMAL INDICATIONS AND ASSESS NEED FOR ACTION!",
        "主警戒触发，逐个检查系统页面确认非正常参数来源！",
    ],
    "TERRAIN": [
        "地形！前方地形！{agl:.0f}英尺，{vs:.0f}fpm下降！立即改出！",
        "CAUTION TERRAIN! {agl:.0f}ft AGL descending at {vs:.0f}fpm! CLIMB!",
        "地形警告！{agl:.0f}英尺AGL！下降率{vs:.0f}fpm！拉起飞越地形！",
    ],
    "SINK_RATE": [
        "SINK RATE过大！{vs:.0f}fpm，{agl:.0f}英尺！带杆减小下降率！",
        "注意：垂直速度{vs:.0f}英尺/分钟偏大！当前{agl:.0f}英尺，柔和修正！",
        "HIGH SINK RATE! {vs:.0f}fpm at {agl:.0f}ft! ADJUST PITCH ATTITUDE!",
    ],
    "TOO_LOW_GEAR": [
        "GEAR DISAGREE! {agl:.0f}ft, gear not down! LOWER GEAR HANDLE!",
        "着陆构型不完整！{agl:.0f}英尺未放起落架！立即放下起落架手柄！",
        "起落架位置不一致！高度{agl:.0f}英尺！核实起落架手柄在DOWN位！",
    ],
    "TOO_LOW_FLAPS": [
        "着陆襟翼未设定！{agl:.0f}英尺襟翼仅{flap:.0%}！设置襟翼30或40！",
        "FLAPS EXTEND! {agl:.0f}ft, flaps {flap:.0%} insufficient! SET FLAPS 30!",
        "进近襟翼不到位！{agl:.0f}英尺{flap:.0%}襟翼，着陆需襟翼30/40！",
    ],
    "GLIDESLOPE": [
        "下滑道偏差{cdi:.1f}点！{agl:.0f}英尺！检查航道偏离，修正下滑角！",
        "ILS GLIDESLOPE! {cdi:.1f} dots deviation! {agl:.0f}ft! GO AROUND IF UNSTABLE!",
        "下滑道未截获/偏离！偏{cdi:.1f}点，{agl:.0f}英尺！核实ILS频率和航道！",
    ],
    "BANK_ANGLE": [
        "坡度角{roll:.0f}度超35度限制！{agl:.0f}英尺低空！立即改平！",
        "EXCESSIVE BANK! {roll:.0f}° at {agl:.0f}ft AGL! REDUCE BANK ANGLE NOW!",
        "大坡度警告！{roll:.0f}度坡度，{agl:.0f}英尺高度！反方向压杆！",
    ],
    "OVERSPEED": [
        "高速警告！{ias:.0f}节超过Vmo/Mmo！收油门至慢车，伸出减速板！",
        "VMO EXCEEDANCE! {ias:.0f}kts! REDUCE TO BELOW 340 KIAS IMMEDIATELY!",
        "超速！{ias:.0f}节超最大使用速度！检查俯仰姿态，收油门减速！",
    ],
    "STALL": [
        "STALL! STALL! {ias:.0f}kts below stall speed! NOSE DOWN, MAX POWER!",
        "失速！{ias:.0f}节低于最小机动速度！立即推杆减小迎角！",
        "失速预警！{ias:.0f}节！推杆至地平线以下，TOGA推力改出！",
    ],
    "MINIMUMS": [
        "决断！{agl:.0f}英尺决断高度！跑道目视则继续，否则复飞！",
        "DECISION HEIGHT {agl:.0f}ft! RUNWAY IN SIGHT? LAND or GO AROUND!",
        "MINIMUMS! {agl:.0f}ft! 决断高到达，执行决断！",
    ],
}

EXTRA_SYSTEM_TEMPLATES = {
    "ENG_OVERHEAT": [
        "发动机排气温度过高！EGT {egt_max:.0f}°C！减小该发推力至EGT回落！",
        "ENGINE OVERHEAT! EGT {egt_max:.0f}°C! REDUCE N1 AND MONITOR TREND!",
        "超温持续！EGT {egt_max:.0f}°C超850°C限制！如不减，考虑关发！",
    ],
    "ENG_ASYM": [
        "双发推力不平衡！N1差值{n1_diff:.1f}%！人工匹配油门位置！",
        "THRUST ASYMMETRY! {n1_diff:.1f}% N1 split! MANUALLY SYNCHRONIZE THROTTLES!",
        "左右推力偏差异常！N1偏差{n1_diff:.1f}%超过5%！检查燃油控制组件！",
    ],
    "FUEL_IMBALANCE": [
        "油箱燃油分配不均！偏差约{fuel_imb:.0f}磅！开启燃油交输活门！",
        "FUEL IMBALANCE {fuel_imb:.0f}lbs! OPEN CROSSFEED VALVE TO BALANCE!",
        "左右燃油量偏差{fuel_imb:.0f}磅，执行燃油平衡程序，监控消耗！",
    ],
    "OIL_PRESS_LOW": [
        "滑油压力低于正常！{oil_press:.0f}psi！核实滑油量和温度指示！",
        "OIL PRESSURE CRITICAL! {oil_press:.0f}psi! POSSIBLE OIL LEAK OR PUMP FAILURE!",
        "滑油压力骤降！{oil_press:.0f}psi低于25psi最低限制！准备关发！",
    ],
    "CABIN_ALT_HIGH": [
        "座舱失压！舱内高度{cabin_alt:.0f}英尺！执行紧急下降程序！",
        "CABIN DECOMPRESSION! {cabin_alt:.0f}ft cabin altitude! EMERGENCY DESCENT!",
        "座舱增压丧失！{cabin_alt:.0f}英尺！戴上氧气面罩！紧急下降至10000ft！",
    ],
    "BUS_VOLT_ABNORM": [
        "DC总线电压{bus_volts:.1f}V异常！检查发电机和电瓶汇流条！",
        "ELECTRICAL FAULT! BUS {bus_volts:.1f}V outside normal range! CHECK GEN SWITCHES!",
        "电源异常！总线电压{bus_volts:.1f}V偏离28V标称值！核实电源配置！",
    ],
    "TAKEOFF_CONFIG": [
        "无法起飞！襟翼仅{flap:.0%}未达到起飞构型！手柄置于襟翼1以上！",
        "CONFIG TAKEOFF WARNING! {flap:.0%} flaps insufficient for takeoff!",
        "起飞构型错误！襟翼{flap:.0%}！起飞至少需襟翼1/2/5！",
    ],
    "LOW_FUEL": [
        "燃油严重不足！{fuel:.0f}磅，~{hours:.1f}小时！立即向ATC宣布最低油量！",
        "FUEL CRITICAL! {fuel:.0f}lbs ~{hours:.1f}hrs! DECLARE MINIMUM FUEL WITH ATC!",
        "剩余油量不足！仅{fuel:.0f}磅！优先备降最近机场，执行节油程序！",
    ],
    "ICING_CONDITION": [
        "机身结冰风险！OAT {oat:.0f}°C，目视可见积冰！全面开启防冰！",
        "ICE ACCUMULATION LIKELY! OAT {oat:.0f}°C! ACTIVATE ALL ANTI-ICE SYSTEMS!",
        "积冰条件！外界{oat:.0f}°C低温+可见水汽！翼/发动机防冰全开！",
    ],
    "APU_FIRE": [
        "APU火警动作！EGT {apu_egt:.0f}°C！APU灭火手柄—拉出—旋转释放！",
        "APU FIRE! EGT {apu_egt:.0f}°C! PULL AND ROTATE APU FIRE HANDLE!",
        "APU舱火警探测！EGT {apu_egt:.0f}°C异常飙升！执行APU地面/空中灭火程序！",
    ],
}

EXTRA_PHASE_TEMPLATES = {
    "TAKEOFF": [
        "起飞推力调定：{n1_0:.0f}%N1，速度{ias:.0f}节增速正产，{agl:.0f}英尺正上升。",
        "TAKEOFF ROLL COMPLETE: airborne {agl:.0f}ft, {ias:.0f}kts, gear retracting.",
    ],
    "CLIMB1": [
        "初始爬升正常：通过{alt:.0f}英尺，{ias:.0f}节爬升，襟翼按计划收上。",
        "CLIMB1 PHASE: {alt:.0f}ft, {ias:.0f}KIAS climbing, flaps {flap:.0%} retracting.",
    ],
    "CLIMB2": [
        "高空爬升中：{alt:.0f}英尺→目标巡航高度，马赫{mach:.2f}增速中。",
        "CONTINUED CLIMB: {alt:.0f}ft to cruise, M{mach:.2f}, OAT {oat:.0f}C.",
    ],
    "CRUISE": [
        "巡航中一切正常：FL{fl:.0f}，自动驾驶/自动油门接通，系统参数绿色范围。",
        "STEADY CRUISE: FL{fl:.0f}, all systems nominal, fuel {fuel:.0f}lbs remaining.",
    ],
    "DESCENT": [
        "下降准备就绪：{alt:.0f}英尺，{ias:.0f}节，下降率{vs:.0f}fpm，预计进近。",
        "DESCENT PHASE: leaving FL{fl:.0f}, {ias:.0f}KIAS, {vs:.0f}fpm rate.",
    ],
    "APPROACH": [
        "稳定进近中：{agl:.0f}英尺AGL，{ias:.0f}节，下滑道{cdi:.1f}点，构型正常。",
        "ON FINAL APPROACH: {agl:.0f}ft, {ias:.0f}kts, flaps {flap:.0%}, gear {gear_status}.",
    ],
    "LANDING": [
        "着陆动作：{agl:.0f}英尺拉平，{ias:.0f}节，反推预位，自动刹车设定。",
        "LANDING FLARE: {agl:.0f}ft, {ias:.0f}kts, thrust idle, flare initiated.",
    ],
    "TAXI": [
        "滑行中：{gs:.0f}节地速，APU运行正常，襟翼收起，准备停靠。",
        "TAXI TO GATE: {gs:.0f}kts ground speed, APU powering, flaps up.",
    ],
}

EXTRA_NORMAL_TEMPLATES = [
    "例行检查：{phase}阶段，{alt:.0f}英尺，双发参数对称正常，无系统告警。",
    "ROUTINE CHECK: {phase}, all parameters within normal limits, no alerts active.",
    "系统巡检正常：N1 {n1_0:.0f}/{n1_1:.0f}%，EGT {egt_0:.0f}/{egt_1:.0f}C，燃油{fuel:.0f}磅，无异常。",
    "ALL SYSTEMS GREEN: {phase} at {alt:.0f}ft, no malfunctions detected.",
]

# =============================================================================
# Helper: get all templates for a given alert name
# =============================================================================

def get_templates(alert_name: str):
    """Get list of text templates for an alert or system rule, including extras."""
    templates = []
    if alert_name in GPWS_TEMPLATES:
        templates.extend(GPWS_TEMPLATES[alert_name])
    if alert_name in EXTRA_GPWS_TEMPLATES:
        templates.extend(EXTRA_GPWS_TEMPLATES[alert_name])
    if alert_name in SYSTEM_TEMPLATES:
        templates.extend(SYSTEM_TEMPLATES[alert_name])
    if alert_name in EXTRA_SYSTEM_TEMPLATES:
        templates.extend(EXTRA_SYSTEM_TEMPLATES[alert_name])
    if alert_name in DERIVED_ALERT_TEMPLATES:
        templates.extend(DERIVED_ALERT_TEMPLATES[alert_name])
    if alert_name in DREF_ALERT_TEMPLATES:
        templates.extend(DREF_ALERT_TEMPLATES[alert_name])
    return templates


def get_phase_template(phase_name: str):
    """Get list of text templates for a flight phase, including extras."""
    templates = list(PHASE_TEMPLATES.get(phase_name, []))
    if phase_name in EXTRA_PHASE_TEMPLATES:
        templates.extend(EXTRA_PHASE_TEMPLATES[phase_name])
    return templates


def get_normal_template():
    """Get normal cruise templates, including extras."""
    return NORMAL_TEMPLATES + EXTRA_NORMAL_TEMPLATES


def get_phase_template(phase_name: str):
    """Get list of text templates for a flight phase."""
    return PHASE_TEMPLATES.get(phase_name, [])


def get_normal_template():
    """Get normal cruise templates."""
    return NORMAL_TEMPLATES


def get_combined_template():
    """Get combined (multi-alert) templates."""
    return COMBINED_TEMPLATES


# =============================================================================
# Self-test
# =============================================================================

if __name__ == "__main__":
    print("=== Text Variants — Summary ===\n")

    total = 0
    for name, templates in GPWS_TEMPLATES.items():
        print(f"  GPWS {name:<20}: {len(templates)} variants")
        total += len(templates)
    print(f"  → GPWS total: {total} templates\n")

    total2 = 0
    for name, templates in SYSTEM_TEMPLATES.items():
        print(f"  SYS  {name:<20}: {len(templates)} variants")
        total2 += len(templates)
    print(f"  → System total: {total2} templates\n")

    total3 = 0
    for name, templates in PHASE_TEMPLATES.items():
        print(f"  PHASE {name:<20}: {len(templates)} variants")
        total3 += len(templates)
    print(f"  → Phase total: {total3} templates\n")

    print(f"  NORMAL: {len(NORMAL_TEMPLATES)} templates")
    print(f"  COMBINED: {len(COMBINED_TEMPLATES)} templates")
    print(f"\n  GRAND TOTAL: {total + total2 + total3 + len(NORMAL_TEMPLATES) + len(COMBINED_TEMPLATES)} templates")
