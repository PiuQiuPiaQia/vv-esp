#!/usr/bin/env python3
"""
构建 assets 资源包
将 xiaozhi-fonts 的 CBIN 字体文件打包为 assets.bin
"""

import os
import sys
import struct
import shutil

def find_xiaozhi_fonts_path():
    """查找 xiaozhi-fonts 组件路径"""
    # 常见的组件路径
    possible_paths = [
        "managed_components/78__xiaozhi-fonts",
        "managed_components/xiaozhi-fonts",
        "../managed_components/78__xiaozhi-fonts",
        "../managed_components/xiaozhi-fonts",
    ]

    for path in possible_paths:
        full_path = os.path.join(os.path.dirname(__file__), path)
        if os.path.exists(full_path):
            return full_path

    return None

def copy_font_files(fonts_path, target_dir):
    """复制需要的字体文件"""
    os.makedirs(target_dir, exist_ok=True)

    # 需要的字体文件
    font_files = [
        "font_puhui_common_14_1.bin",
        "font_puhui_common_20_4.bin",
    ]

    cbin_dir = os.path.join(fonts_path, "cbin")
    if not os.path.exists(cbin_dir):
        print(f"错误: 找不到 cbin 目录: {cbin_dir}")
        return False

    copied = []
    for font_file in font_files:
        src = os.path.join(cbin_dir, font_file)
        dst = os.path.join(target_dir, font_file)

        if os.path.exists(src):
            shutil.copy2(src, dst)
            size = os.path.getsize(dst)
            print(f"  ✓ 复制: {font_file} ({size:,} bytes)")
            copied.append(font_file)
        else:
            print(f"  ✗ 未找到: {font_file}")

    return len(copied) > 0

def pack_assets(assets_dir, output_file):
    """打包资源文件为 assets.bin (esp_mmap_assets 格式)"""
    files = sorted([f for f in os.listdir(assets_dir) if os.path.isfile(os.path.join(assets_dir, f))])

    if not files:
        print("错误: 没有文件可打包")
        return False

    print(f"\n打包 {len(files)} 个文件...")

    # 构建文件数据和文件表
    merged_data = bytearray()
    file_info_list = []

    for filename in files:
        file_path = os.path.join(assets_dir, filename)
        file_size = os.path.getsize(file_path)

        # 记录文件信息 (文件名, 偏移, 大小, 宽度, 高度)
        file_info_list.append((filename, len(merged_data), file_size, 0, 0))

        # 添加魔数 "ZZ" (0x5A5A)
        merged_data.extend(b'\x5A\x5A')

        # 添加文件数据
        with open(file_path, 'rb') as f:
            merged_data.extend(f.read())

        print(f"  + {filename} @ offset {len(merged_data) - file_size - 2}, size {file_size}")

    # 构建文件表 (每项 44 字节)
    mmap_table = bytearray()
    for filename, offset, file_size, width, height in file_info_list:
        # 文件名 (32字节, null填充)
        name_bytes = filename.encode('utf-8')[:32].ljust(32, b'\x00')
        mmap_table.extend(name_bytes)

        # 文件大小 (4字节, 小端)
        mmap_table.extend(struct.pack('<I', file_size))

        # 文件偏移 (4字节, 小端)
        mmap_table.extend(struct.pack('<I', offset))

        # 宽度和高度 (各2字节, 小端)
        mmap_table.extend(struct.pack('<HH', width, height))

    # 组合文件表和数据
    combined_data = mmap_table + merged_data

    # 计算校验和
    checksum = sum(combined_data) & 0xFFFFFFFF

    # 构建最终文件头 (12字节)
    header = bytearray()
    header.extend(struct.pack('<I', len(files)))       # 文件数量
    header.extend(struct.pack('<I', checksum))          # 校验和
    header.extend(struct.pack('<I', len(combined_data))) # 数据长度

    # 写入输出文件
    with open(output_file, 'wb') as f:
        f.write(header + combined_data)

    output_size = os.path.getsize(output_file)
    print(f"\n✓ 资源包已生成: {output_file}")
    print(f"  文件数: {len(files)}")
    print(f"  总大小: {output_size:,} bytes ({output_size / 1024:.2f} KB)")
    print(f"  校验和: 0x{checksum:08X}")

    return True

def main():
    print("=" * 60)
    print("构建 assets 资源包")
    print("=" * 60)
    print()

    # 1. 查找 xiaozhi-fonts 组件
    print("1. 查找 xiaozhi-fonts 组件...")
    fonts_path = find_xiaozhi_fonts_path()

    if fonts_path is None:
        print("错误: 找不到 xiaozhi-fonts 组件")
        print("请先运行: idf.py reconfigure")
        sys.exit(1)

    print(f"  ✓ 找到: {fonts_path}")
    print()

    # 2. 创建临时目录并复制字体
    print("2. 复制字体文件...")
    temp_dir = os.path.join(os.path.dirname(__file__), "build_assets_temp")

    if not copy_font_files(fonts_path, temp_dir):
        print("错误: 无法复制字体文件")
        sys.exit(1)
    print()

    # 3. 打包资源
    print("3. 打包资源...")
    output_file = os.path.join(os.path.dirname(__file__), "assets.bin")

    if not pack_assets(temp_dir, output_file):
        print("错误: 打包失败")
        sys.exit(1)

    # 4. 清理临时目录
    shutil.rmtree(temp_dir)

    print()
    print("=" * 60)
    print("完成!")
    print("=" * 60)
    print()
    print("下一步:")
    print("  1. 烧录 assets 分区:")
    print(f"     parttool.py write_partition --partition-name=assets --input={output_file}")
    print()
    print("  2. 或者重新编译整个项目 (如果在 partitions.csv 中配置了 FLASH_IN_PROJECT):")
    print("     idf.py build flash")

if __name__ == "__main__":
    main()
