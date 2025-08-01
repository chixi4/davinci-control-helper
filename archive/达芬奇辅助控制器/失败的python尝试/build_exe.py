import PyInstaller.__main__
import os
import sys

def build_exe():
    # 获取当前目录
    current_dir = os.path.dirname(os.path.abspath(__file__))
    
    # PyInstaller参数
    args = [
        'davinci_drag_gui.py',
        '--onefile',
        '--windowed',
        '--name=达芬奇双鼠标拖拽助手',
        '--distpath=dist',
        '--workpath=build',
        '--specpath=.',
        '--add-data=find_mouse_id.py;.',
        '--hidden-import=win32api',
        '--hidden-import=win32con',
        '--hidden-import=win32gui',
        '--hidden-import=win32process',
        '--hidden-import=ctypes.wintypes',
        '--clean',
        '--noconfirm'
    ]
    
    # 如果有图标文件，添加图标参数
    icon_path = os.path.join(current_dir, 'icon.ico')
    if os.path.exists(icon_path):
        args.extend(['--icon', icon_path])
    
    print("开始打包...")
    print(f"参数: {' '.join(args)}")
    
    try:
        PyInstaller.__main__.run(args)
        print("\n打包完成！")
        print(f"可执行文件位于: {os.path.join(current_dir, 'dist', '达芬奇双鼠标拖拽助手.exe')}")
        
        # 复制find_mouse_id.py到dist目录
        import shutil
        src_file = os.path.join(current_dir, 'find_mouse_id.py')
        dst_file = os.path.join(current_dir, 'dist', 'find_mouse_id.py')
        if os.path.exists(src_file):
            shutil.copy2(src_file, dst_file)
            print(f"已复制 find_mouse_id.py 到 dist 目录")
            
    except Exception as e:
        print(f"打包失败: {e}")
        return False
    
    return True

if __name__ == "__main__":
    build_exe()