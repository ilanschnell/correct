from glob import glob
from setuptools import Extension, setup


sources = sorted(glob("src/*/*.c"))
sources.append("correct.c")

headers = sorted(glob("src/include/**/*.h", recursive=True))

setup(
    name="correct",
    version="0.1.0",
    ext_modules=[
        Extension(
            "correct",
            sources=sources,
            include_dirs=["src/include"],
            depends=headers,
        ),
    ],
    py_modules=["test_correct"],
    install_requires=["bitarray >=3.4"],
)
