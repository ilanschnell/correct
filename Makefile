.PHONY: all clean test


correct.so: correct.c
	python setup.py build_ext --inplace


test: correct.so
	python test_correct.py


clean:
	rm -rf build dist
	rm -f *.o *.so
	rm -rf __pycache__ *.egg-info
