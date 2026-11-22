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
    author = "Ilan Schnell",
    author_email = "ilanschnell@gmail.com",
    url = "https://github.com/ilanschnell/correct",
    license = "BSD",
    license_files = ["LICENSE", "libcorrect/LICENSE"],
    python_requires = ">=3.9",
    classifiers = [
        "Development Status :: 3 - Alpha",
        "Intended Audience :: Developers",
        "Operating System :: OS Independent",
        "Programming Language :: C",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.9",
        "Programming Language :: Python :: 3.10",
        "Programming Language :: Python :: 3.11",
        "Programming Language :: Python :: 3.12",
        "Programming Language :: Python :: 3.13",
        "Programming Language :: Python :: 3.14",
        "Programming Language :: Python :: 3.15",
        "Topic :: Utilities",
    ],
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
