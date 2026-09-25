// ane_request_capture.m - dump the IOSurfaces the macOS ANE runtime actually hands
// the engine, around every evaluation made in this process.
//
// Build (macOS):
//   clang -dynamiclib -fobjc-arc -framework Foundation -framework IOSurface \
//     ane_request_capture.m -o libane_request_capture.dylib
// Use: ctypes.CDLL(path) (the constructor swizzles), then
//   anecap_arm(out_dir, n_evaluations)
// Each captured evaluation k writes out_dir/eval_KKK.json (model attributes'
// LiveInput/LiveOutput lists + per-surface IOSurface properties) and the raw
// surface bytes out_dir/eval_KKK_{in,out}_I.bin (inputs before, outputs after).
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <math.h>

static NSString *g_dir;
static int g_left, g_seq;
static __thread int g_depth;  // nested hooked calls capture once, at the outermost

static id call0(id obj, const char *sel) {
  SEL s = sel_registerName(sel);
  if (!obj || ![obj respondsToSelector:s]) return nil;
  return ((id (*)(id, SEL))objc_msgSend)(obj, s);
}

static NSDictionary *surface_props(IOSurfaceRef s) {
  return @{
    @"width" : @(IOSurfaceGetWidth(s)), @"height" : @(IOSurfaceGetHeight(s)),
    @"bytes_per_row" : @(IOSurfaceGetBytesPerRow(s)),
    @"bytes_per_element" : @(IOSurfaceGetBytesPerElement(s)),
    @"plane_count" : @(IOSurfaceGetPlaneCount(s)),
    @"alloc_size" : @(IOSurfaceGetAllocSize(s)),
    @"pixel_format" : @(IOSurfaceGetPixelFormat(s)),
  };
}

static NSArray *dump_surfaces(NSArray *objs, NSString *tag, int seq) {
  NSMutableArray *rows = [NSMutableArray array];
  for (NSUInteger i = 0; i < objs.count; i++) {
    id o = objs[i];
    IOSurfaceRef s = (__bridge IOSurfaceRef)call0(o, "ioSurface");
    NSUInteger start = ((NSUInteger (*)(id, SEL))objc_msgSend)(o, sel_registerName("startOffset"));
    if (!s) { [rows addObject:@{@"missing" : @YES}]; continue; }
    NSMutableDictionary *r = [surface_props(s) mutableCopy];
    r[@"start_offset"] = @(start);
    kern_return_t lk = IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    if (lk != kIOReturnSuccess) {
      r[@"lock_error"] = @(lk);
      [rows addObject:r];
      continue;
    }
    // bytes_per_row * height, not alloc_size: the CPU mapping can end before the
    // 16 KB-rounded allocation (reading alloc_size faulted on staged-Qwen surfaces)
    size_t len = IOSurfaceGetBytesPerRow(s) * IOSurfaceGetHeight(s);
    NSData *d = [NSData dataWithBytes:IOSurfaceGetBaseAddress(s) length:len];
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    NSString *f = [NSString stringWithFormat:@"eval_%03d_%@_%lu.bin", seq, tag, (unsigned long)i];
    [d writeToFile:[g_dir stringByAppendingPathComponent:f] atomically:NO];
    r[@"file"] = f;
    [rows addObject:r];
  }
  return rows;
}

static id jsonable(id v) {
  if ([v isKindOfClass:[NSDictionary class]]) {
    NSMutableDictionary *o = [NSMutableDictionary dictionary];
    for (id k in v) o[[k description]] = jsonable(v[k]);
    return o;
  }
  if ([v isKindOfClass:[NSArray class]]) {
    NSMutableArray *o = [NSMutableArray array];
    for (id x in v) [o addObject:jsonable(x)];
    return o;
  }
  if ([v isKindOfClass:[NSNumber class]])
    return isfinite([v doubleValue]) ? v : (id)[v description];  // NaN/inf are not JSON
  if ([v isKindOfClass:[NSString class]]) return v;
  return [v description];
}

static int begin(id model, id request, NSMutableDictionary **rec) {
  if (g_depth++ || g_left <= 0) return -1;
  int seq = g_seq++;
  g_left--;
  NSMutableDictionary *r = [NSMutableDictionary dictionary];
  r[@"seq"] = @(seq);
  r[@"model"] = [model description] ?: @"";
  id attrs = call0(model, "modelAttributes");
  if (attrs) r[@"model_attributes"] = jsonable(attrs);
  r[@"input_indices"] = jsonable(call0(request, "inputIndexArray") ?: @[]);
  r[@"output_indices"] = jsonable(call0(request, "outputIndexArray") ?: @[]);
  r[@"inputs"] = dump_surfaces(call0(request, "inputArray"), @"in", seq);
  *rec = r;
  return seq;
}

static void finish(int seq, id request, NSMutableDictionary *r) {
  g_depth--;
  if (seq < 0) return;
  r[@"outputs"] = dump_surfaces(call0(request, "outputArray"), @"out", seq);
  NSData *j = [NSJSONSerialization dataWithJSONObject:r options:NSJSONWritingPrettyPrinted error:nil];
  [j writeToFile:[g_dir stringByAppendingPathComponent:[NSString stringWithFormat:@"eval_%03d.json", seq]]
      atomically:NO];
}

// -[_ANEClient evaluateWithModel:options:request:qos:error:] and the direct variant
typedef BOOL (*eval5_t)(id, SEL, id, id, id, unsigned, id *);
static eval5_t o_eval, o_direct;
static BOOL h_eval(id self, SEL cmd, id model, id opts, id req, unsigned qos, id *err) {
  NSMutableDictionary *r = nil;
  int seq = begin(model, req, &r);
  BOOL ok = o_eval(self, cmd, model, opts, req, qos, err);
  finish(seq, req, r);
  return ok;
}
static BOOL h_direct(id self, SEL cmd, id model, id opts, id req, unsigned qos, id *err) {
  NSMutableDictionary *r = nil;
  int seq = begin(model, req, &r);
  BOOL ok = o_direct(self, cmd, model, opts, req, qos, err);
  finish(seq, req, r);
  return ok;
}

// -[_ANEProgramForEvaluation processRequest:model:qos:qIndex:modelStringID:options:returnValue:error:]
typedef BOOL (*proc_t)(id, SEL, id, id, unsigned, unsigned, unsigned long long, id, void *, id *);
static proc_t o_proc;
static BOOL h_proc(id self, SEL cmd, id req, id model, unsigned qos, unsigned qidx,
                   unsigned long long sid, id opts, void *rv, id *err) {
  NSMutableDictionary *r = nil;
  int seq = begin(model, req, &r);
  BOOL ok = o_proc(self, cmd, req, model, qos, qidx, sid, opts, rv, err);
  finish(seq, req, r);
  return ok;
}

static IMP swap(const char *cls, const char *sel, IMP repl) {
  Class c = objc_getClass(cls);
  Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
  if (!m) { fprintf(stderr, "anecap: %s %s not found\n", cls, sel); return NULL; }
  return method_setImplementation(m, repl);
}

__attribute__((constructor)) static void anecap_init(void) {
  dlopen("/System/Library/PrivateFrameworks/AppleNeuralEngine.framework/AppleNeuralEngine", RTLD_NOW);
  o_eval = (eval5_t)swap("_ANEClient", "evaluateWithModel:options:request:qos:error:", (IMP)h_eval);
  o_direct = (eval5_t)swap("_ANEClient", "doEvaluateDirectWithModel:options:request:qos:error:", (IMP)h_direct);
  o_proc = (proc_t)swap("_ANEProgramForEvaluation",
                        "processRequest:model:qos:qIndex:modelStringID:options:returnValue:error:",
                        (IMP)h_proc);
}

void anecap_arm(const char *dir, int n) {
  g_dir = [NSString stringWithUTF8String:dir];
  [[NSFileManager defaultManager] createDirectoryAtPath:g_dir withIntermediateDirectories:YES
                                             attributes:nil error:nil];
  g_left = n;
  g_seq = 0;
}

int anecap_count(void) { return g_seq; }
