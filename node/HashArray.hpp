#ifndef ZT_HASHARRAY_HPP
#define ZT_HASHARRAY_HPP

#include <stdint.h>
#include <string.h>

#include <map>
#include <vector>
#include <list>
#include <assert.h>

namespace ZeroTier {

/*
Closed Hash Map of pointers.
Two pointers are used to mark an element empty (0x0 and 0x1)
Table size if power 2, since the module operation is very cheap for these sizes.
x % y == x & (y - 1); for y = 2^n

erase does not reallocate - there is compact() for this
- fast add and removal of elements
- fast iteration

TODO:
	- reduce min capacity to 0
	- use pyDict pertub
	- use key of long long int or unsigned int if requested?
	- fold _capacity into _size
*/
template<typename T>
class HashArray {

#define free_ptr ((T) 0)
#define dummy_ptr ((T) 1)
#define min_capacity 8

public:

	class iterator : public std::iterator<std::output_iterator_tag, T>
	{
		friend HashArray<T>;

		public:
		typedef T value_type;

		iterator(T *data = NULL) : _data(data) {
		}

		iterator(const iterator &iter) : _data(iter._data) {
		}

		bool operator==(iterator const& rhs) const {
			return (*_data == *rhs._data);
		}

		bool operator!=(iterator const& rhs) const {
			return !(*this==rhs);
		}

		iterator& operator++() {
			do {
				++_data;
			} while(*_data <= dummy_ptr);

			return *this;
		}

		iterator operator++(int) {
			iterator tmp(*this);
			operator++();
			return tmp;
		}

		T& operator*() {
			return *_data;
		}

		T& operator*() const {
			return *_data;
		}

		T& operator->() {
			return *_data;
		}

		const T& operator->() const {
			return *_data;
		}

	private:
		T *_data;
	};

	typedef const iterator const_iterator;

	HashArray() :
		_data(NULL), _size(0), _capacity(0)
	{
		printf("HashArray()\n");
		resize(min_capacity);
	}

	HashArray(const HashArray &a) :
		_data(NULL), _size(a._size), _capacity(a._capacity)
	{
		_data = new T[_capacity + 1];
		memcpy(_data, a._data, sizeof(T) * (a._capacity + 1));
	}

	virtual ~HashArray() {
		printf("~HashArray()\n");
		delete[] _data;
	}

	template<typename K>
	bool erase(const K &key) {
		const size_t mask = (_capacity - 1);
		size_t p = (key.hash() & mask);
		assert(key.hash() > (T) (void*) dummy_ptr);

		while(true) {
			if(_data[p] > dummy_ptr && key == *_data[p]) {
				_data[p] = (T) (void*) dummy_ptr;
				--_size;
				return true;
			}

			if(_data[p] == free_ptr) {
				return false;
			}

			p = ((p + 1) & mask);
		}
	}

	bool erase(const iterator &iter) {
		if(*iter._data > dummy_ptr) {
			*iter._data = (T) (void*) dummy_ptr;
			--_size;
			return true;
		} else {
			return false;
		}
	}

	template<typename K>
	const iterator find(const K &key) const {
		const size_t mask = (_capacity - 1);
		size_t p = (key.hash() & mask);

		while(true) {
			if(_data[p] > dummy_ptr && key == *_data[p]) {
					return iterator(&_data[p]);
			}

			if(_data[p] == free_ptr) {
				return end();
			}

			p = ((p + 1) & mask);
		}
	}

	template<typename K>
	bool set(const K &key, const T &value) {
		const size_t mask = (_capacity - 1);
		size_t p = (key.hash() & mask);

		while(true) {
			if(_data[p] > dummy_ptr && key == *_data[p]) {
				return false;
			}

			if(_data[p] <= dummy_ptr) {
				_data[p] = value;
				_size++;
				grow();
				return true;
			}

			p = ((p + 1) & mask);
		}
	}

	inline iterator begin() {
		return iterator(next(0));
	}

    inline iterator begin() const {
		return iterator(next(0));
	}

    inline iterator end() {
		return iterator(_data + _capacity);
	}

    inline iterator end() const {
		return iterator(_data + _capacity);
	}

	inline size_t size() {
		return _size;
	}

	inline size_t capacity() {
		return _capacity;
	}

	inline bool empty() {
		return (_size == 0);
	}

	inline bool operator==(const HashArray &k) const throw()
	{
		if(k.size() != size())
			return false;

		//hm, the hash might not be equal..., still fullfills equality
		for(unsigned long i=0;i<size();++i) {
			if (_data[i] > dummy_ptr && _data[i] != k._data[i])
				return false;
		}
		return true;
	}

	void compact() {
		shrink();
	}

private:

	inline bool grow() {
		// > 87%
		if(_size > ((_capacity / 2) + (_capacity / 4) + (_capacity / 8)))  {
			//new load will be >= 45%
			resize(_capacity << 1);
			return true;
		}
		return false;
	}

	inline bool shrink() {
		// < 50%
		if(_capacity > min_capacity && _size < (_capacity / 2)) {
			//new load will be < 50%
			resize(_capacity >> 1);
			return true;
		}
		return false;
	}

	void resize(unsigned int new_capacity) {
		assert(new_capacity >= min_capacity);
		assert(is_power_of_2(new_capacity));
		/*
		if(new_capacity == 0) {
			free(_data);
			_data = NULL;
			_capacity = 0;
			_size = 0;
			return;
		}*/

		/* allocate memory that is nulled */
		T *new_data = new T[new_capacity+1]();

		if(new_data == NULL) {
			return;
		}

		//we set an end marker to stop iteration
		new_data[new_capacity] = (T) ~0;

		const size_t new_mask = (new_capacity - 1);
		for(unsigned int k = 0; k < _capacity; ++k) {
			if(_data[k] <= dummy_ptr)
				continue;

			const size_t hash = _data[k]->hash();
			size_t p = (hash & new_mask);
			while(true) {
				if(new_data[p] == free_ptr) {
					new_data[p] = _data[k];
					break;
				}
				p = ((p + 1) & new_mask);
			}
		}

		delete[] _data;
		_data = new_data;
		_capacity = new_capacity;
	}

	T* next(size_t idx) const {
		while(idx < _capacity) {
			if(_data[idx] > dummy_ptr) {
				return &_data[idx];
			}
			++idx;
		}
		return &_data[_capacity];
	}

	inline bool is_power_of_2(unsigned int n) {
		return n && !(n & (n - 1));
	}

	T *_data;
	unsigned int _size;
	unsigned int _capacity;
};

} // namespace ZeroTier

#endif
