import re
from glob import glob
from setuptools import Extension, setup


kwds = {}

# Read version from correct.c
pat = re.compile(r'#define\s+CORRECT_VERSION\s+"(\S+)"', re.M)
data = open('correct.c').read()
kwds['version'] = pat.search(data).group(1)

sources = sorted(glob("libcorrect/src/*/*.c"))
sources.append("correct.c")

headers = sorted(glob("libcorrect/include/**/*.h", recursive=True))

setup(
    name = "correct",
    license_files = ["libcorrect/LICENSE"],
    ext_modules=[
        Extension(
            "correct",
            sources = sources,
            include_dirs = ["libcorrect/include"],
            depends = headers,
        ),
    ],
    py_modules = ["test_correct"],
    install_requires = ["bitarray >=3.4"],
    **kwds
)
