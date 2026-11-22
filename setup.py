from setuptools import Extension, setup


setup(
    name="correct",
    version="0.1.0",
    ext_modules=[
        Extension(
            "correct",
            sources=["correct.c"],
            extra_objects=["libcorrect.a"],
        ),
    ],
    py_modules="test_correct",
    install_requires=["bitarray >=3.4"],
)
