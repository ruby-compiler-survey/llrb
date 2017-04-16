#ifndef LLRUBY_ISEQ_COMPILER_H
#define LLRUBY_ISEQ_COMPILER_H

#include "llruby/iseq.h"

namespace llruby {

class IseqCompiler {
 private:
  const Iseq& iseq;

 public:
  IseqCompiler(const Iseq& value):iseq(value) {};
  void Compile();
}; // class IseqCompiler

}; // namespace llruby

#endif // LLRUBY_ISEQ_COMPILER_H