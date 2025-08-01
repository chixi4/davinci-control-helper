import ctypes

# 紧急恢复鼠标速度到默认值
try:
    default_speed = 10
    ctypes.windll.user32.SystemParametersInfoW(0x0071, 0, default_speed, 0)
    print(f"紧急恢复成功！鼠标速度已设置为默认值: {default_speed}")
except Exception as e:
    print(f"恢复失败: {e}")

input("按回车键退出...")