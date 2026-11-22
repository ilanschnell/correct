.PHONY: build clean test


build:
	python setup.py build_ext --inplace


test: build
	python test_correct.py


clean:
	rm -rf build dist
	rm -f *.o *.so
	rm -rf __pycache__ *.egg-info
