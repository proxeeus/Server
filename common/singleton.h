#ifndef EQEMU_COMMON_SINGLETON_H
#define EQEMU_COMMON_SINGLETON_H

namespace EQEmu
{
	template <typename T>
	class Singleton
	{
	public:
		Singleton() { _inst = static_cast<T*>(this); }
		~Singleton() { if(_inst) { delete _inst; } _inst = nullptr; }
		static T& Init() { _inst = new T(); return *_inst; }
		static T& Get() { return *_inst; }
	private:
		Singleton(const Singleton<T>&);
		Singleton& operator=(const Singleton<T>&);
	protected:
		static T* _inst;
	};
}

#endif
