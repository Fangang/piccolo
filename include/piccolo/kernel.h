#ifndef KERNELREGISTRY_H_
#define KERNELREGISTRY_H_

#include "piccolo/table.h"
#include "util/common.h"

#include <boost/function.hpp>
#include <boost/lexical_cast.hpp>

#include <map>

namespace piccolo {

class TableBase;
class Worker;

class KernelBase {
public:
  // Called upon creation of this kernel by a worker.
  virtual void _init() {
  }

  // The table and shard being processed.
  int currentShard() const {
    return shard_;
  }
  int currentTable() const {
    return table_id_;
  }

  ShardedTable* table(int id);

  template<class K, class V>
  TableT<K, V>* table(int id) {
    return dynamic_cast<TableT<K, V>*>(table(id));
  }

private:
  friend class Worker;
  friend class Master;

  void initialize_internal(Worker* w, int table_id, int shard);

  Worker *w_;
  int shard_;
  int table_id_;
};

class DSMKernel: public KernelBase {
public:
};

struct KernelInfo {
  virtual ~KernelInfo() {
  }
  KernelInfo(const char* name) :
      name_(name) {
  }

  virtual KernelBase* create() = 0;
  virtual void Run(KernelBase* obj, const string& method_name) = 0;
  virtual bool has_method(const string& method_name) = 0;

  string name_;
};

template<class C>
struct KernelInfoT: public KernelInfo {
  typedef void (C::*Method)();
  std::map<string, Method> methods_;

  KernelInfoT(const char* name) :
      KernelInfo(name) {
  }

  KernelBase* create() {
    return new C;
  }

  void Run(KernelBase* obj, const string& method_id) {
    boost::function<void(C*)> m(methods_[method_id]);
    m((C*) obj);
  }

  bool has_method(const string& name) {
    return methods_.find(name) != methods_.end();
  }

  void register_method(const char* mname, Method m) {
    methods_[mname] = m;
  }
};

class ConfigData;
class KernelRegistry {
public:
  typedef std::map<string, KernelInfo*> Map;
  Map& kernels() {
    return m_;
  }
  KernelInfo* kernel(const string& name) {
    return m_[name];
  }

  static KernelRegistry* Get();
private:
  KernelRegistry() {
  }
  Map m_;
};

template<class C>
struct KernelRegistrationHelper {
  KernelRegistrationHelper(const char* name) {
    KernelRegistry::Map& kreg = KernelRegistry::Get()->kernels();

    CHECK(kreg.find(name) == kreg.end());
    kreg.insert(std::make_pair(name, new KernelInfoT<C>(name)));
  }
};

template<class C>
struct MethodRegistrationHelper {
  MethodRegistrationHelper(const char* klass, const char* mname,
      void (C::*m)()) {
    ((KernelInfoT<C>*) KernelRegistry::Get()->kernel(klass))->register_method(
        mname, m);
  }
};

#define REGISTER_KERNEL(klass)\
  static KernelRegistrationHelper<klass> k_helper_ ## klass(#klass);

#define REGISTER_METHOD(klass, method)\
  static MethodRegistrationHelper<klass> m_helper_ ## klass ## _ ## method(#klass, #method, &klass::method);

#define REGISTER_RUNNER(klass)

}
#endif /* KERNELREGISTRY_H_ */
