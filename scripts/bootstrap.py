"""下载固定的官方依赖并校验摘要。仅写入仓库的 third_party 目录。

使用标准库，不修改系统安装、PATH 或 Qt。已有依赖也验证来源压缩包，
解包前检查每个成员路径，避免写入目标目录之外。
"""
import hashlib
import io
import pathlib
import urllib.request
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
PACKAGES = [
    ('curl', 'https://curl.se/windows/dl-8.23.0_9/curl-8.23.0_9-win64-mingw.zip',
     'sha256', '89cb71930f4242d884e8e349463455932ab1d089b20f4f99318261a3cbff9167'),
    ('sqlite', 'https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip',
     'sha3_256', '92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30'),
]


def main():
    for name, url, algorithm, expected in PACKAGES:
        data = urllib.request.urlopen(url, timeout=90).read()
        actual = hashlib.new(algorithm, data).hexdigest()
        if actual != expected:
            raise RuntimeError(f'{name}: checksum mismatch, refusing extraction')
        target = (ROOT/'third_party'/name).resolve()
        target.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for member in archive.infolist():
                (target/member.filename).resolve().relative_to(target)
            archive.extractall(target)
        print(f'{name}: {algorithm} verified; {target}')


if __name__ == '__main__':
    main()
