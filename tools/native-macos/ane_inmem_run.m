// ane_inmem_run.m - compile one MIL in process with this Mac's ANE compiler (AppleNeuralEngine
// _ANEInMemoryModel) and time evaluateWithQoS. A second native path for MILs that e5rt refuses.
//
// Build: clang -O2 -fobjc-arc -framework Foundation -framework IOSurface ane_inmem_run.m -o ane_inmem_run
// Run:   TMPDIR=<scratch>/tmp ane_inmem_run MIL WEIGHT_BIN OUT_DIR WARMUP REPEAT \
//          in:FILE:SURFACE_BYTES ... out:NAME:SURFACE_BYTES ...
// Inputs and outputs bind in MIL signature / return order. Each input file's bytes go to the start of
// its surface (zero padding after), so a dense tensor or a single padded row binds as is. Each output
// surface is written whole to OUT_DIR/NAME.bin. stdout: one JSON line with compile, load and per-call ms.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <dlfcn.h>
#import <objc/message.h>

static IOSurfaceRef surface(size_t bytes) {
  NSDictionary *p = @{(id)kIOSurfaceWidth: @(bytes), (id)kIOSurfaceHeight: @1, (id)kIOSurfaceBytesPerElement: @1,
                      (id)kIOSurfaceBytesPerRow: @(bytes), (id)kIOSurfaceAllocSize: @(bytes),
                      (id)kIOSurfacePixelFormat: @0};
  return IOSurfaceCreate((__bridge CFDictionaryRef)p);
}

static double ms_since(CFAbsoluteTime t) { return (CFAbsoluteTimeGetCurrent() - t) * 1e3; }

int main(int argc, char **argv) {
  @autoreleasepool {
    if (argc < 7) {
      fprintf(stderr, "usage: %s MIL WEIGHT_BIN OUT_DIR WARMUP REPEAT in:FILE:BYTES... out:NAME:BYTES...\n", argv[0]);
      return 2;
    }
    if (!dlopen("/System/Library/PrivateFrameworks/AppleNeuralEngine.framework/AppleNeuralEngine", RTLD_NOW)) return 3;
    NSData *mil = [NSData dataWithContentsOfFile:@(argv[1])];
    NSData *weights = [NSData dataWithContentsOfFile:@(argv[2]) options:NSDataReadingMappedIfSafe error:nil];
    NSString *outDir = @(argv[3]);
    int warmup = atoi(argv[4]), repeat = atoi(argv[5]);
    if (!mil || !weights) { fprintf(stderr, "cannot read MIL or weights\n"); return 4; }

    NSMutableArray *ins = [NSMutableArray array], *inIdx = [NSMutableArray array];
    NSMutableArray *outs = [NSMutableArray array], *outIdx = [NSMutableArray array], *outNames = [NSMutableArray array];
    NSMutableArray *outSurfaces = [NSMutableArray array];
    Class surfaceObject = NSClassFromString(@"_ANEIOSurfaceObject");
    for (int i = 6; i < argc; i++) {
      NSArray *f = [@(argv[i]) componentsSeparatedByString:@":"];
      size_t bytes = (size_t)[f[2] longLongValue];
      IOSurfaceRef s = surface(bytes);
      if (!s) { fprintf(stderr, "IOSurfaceCreate %zu failed\n", bytes); return 5; }
      IOSurfaceLock(s, 0, NULL);
      memset(IOSurfaceGetBaseAddress(s), 0, bytes);
      if ([f[0] isEqualToString:@"in"]) {
        NSData *d = [NSData dataWithContentsOfFile:f[1]];
        if (!d || d.length > bytes) { fprintf(stderr, "input %s missing or larger than its surface\n", argv[i]); return 6; }
        memcpy(IOSurfaceGetBaseAddress(s), d.bytes, d.length);
      }
      IOSurfaceUnlock(s, 0, NULL);
      id wrapped = ((id (*)(id, SEL, IOSurfaceRef))objc_msgSend)(surfaceObject, @selector(objectWithIOSurface:), s);
      if ([f[0] isEqualToString:@"in"]) { [inIdx addObject:@(ins.count)]; [ins addObject:wrapped]; }
      else {
        [outIdx addObject:@(outs.count)]; [outs addObject:wrapped]; [outNames addObject:f[1]];
        [outSurfaces addObject:(__bridge id)s];
      }
    }

    NSDictionary *weightMap = @{@"@model_path/weights/weight.bin": @{@"offset": @0, @"data": weights}};
    id descriptor = ((id (*)(Class, SEL, id, id, id))objc_msgSend)(NSClassFromString(@"_ANEInMemoryModelDescriptor"),
        @selector(modelWithMILText:weights:optionsPlist:), mil, weightMap, nil);
    id model = ((id (*)(Class, SEL, id))objc_msgSend)(NSClassFromString(@"_ANEInMemoryModel"),
        @selector(inMemoryModelWithDescriptor:), descriptor);
    NSString *ident = ((id (*)(id, SEL))objc_msgSend)(model, @selector(hexStringIdentifier));
    if (!model || ident.length == 0) { fprintf(stderr, "in-memory model creation failed\n"); return 7; }
    NSString *staging = [NSTemporaryDirectory() stringByAppendingPathComponent:ident];
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm createDirectoryAtPath:[staging stringByAppendingPathComponent:@"weights"] withIntermediateDirectories:YES
                   attributes:nil error:nil];
    [mil writeToFile:[staging stringByAppendingPathComponent:@"model.mil"] atomically:YES];
    [weights writeToFile:[staging stringByAppendingPathComponent:@"weights/weight.bin"] atomically:YES];

    NSError *error = nil;
    int rc = 0;
    CFAbsoluteTime t = CFAbsoluteTimeGetCurrent();
    BOOL ok = ((BOOL (*)(id, SEL, unsigned int, id, NSError **))objc_msgSend)(model,
        @selector(compileWithQoS:options:error:), 21, @{}, &error);
    double compileMs = ms_since(t);
    double loadMs = 0;
    NSMutableArray *times = [NSMutableArray array];
    if (!ok) { fprintf(stderr, "compile: %s\n", error.description.UTF8String); rc = 8; goto done; }
    t = CFAbsoluteTimeGetCurrent();
    ok = ((BOOL (*)(id, SEL, unsigned int, id, NSError **))objc_msgSend)(model, @selector(loadWithQoS:options:error:),
        21, @{}, &error);
    loadMs = ms_since(t);
    if (!ok) { fprintf(stderr, "load: %s\n", error.description.UTF8String); rc = 9; goto done; }
    {
      id request = ((id (*)(Class, SEL, id, id, id, id, id, id, id))objc_msgSend)(NSClassFromString(@"_ANERequest"),
          @selector(requestWithInputs:inputIndices:outputs:outputIndices:weightsBuffer:perfStats:procedureIndex:),
          ins, inIdx, outs, outIdx, nil, nil, @0);
      for (int i = 0; i < warmup + repeat; i++) {
        t = CFAbsoluteTimeGetCurrent();
        ok = ((BOOL (*)(id, SEL, unsigned int, id, id, NSError **))objc_msgSend)(model,
            @selector(evaluateWithQoS:options:request:error:), 21, @{}, request, &error);
        double ms = ms_since(t);
        if (!ok) { fprintf(stderr, "evaluate %d: %s\n", i, error.description.UTF8String); rc = 10; break; }
        if (i >= warmup) [times addObject:@(ms)];
      }
    }
    for (NSUInteger i = 0; rc == 0 && i < outSurfaces.count; i++) {
      IOSurfaceRef s = (__bridge IOSurfaceRef)outSurfaces[i];
      IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
      NSData *d = [NSData dataWithBytes:IOSurfaceGetBaseAddress(s) length:IOSurfaceGetAllocSize(s)];
      IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
      [d writeToFile:[outDir stringByAppendingPathComponent:[outNames[i] stringByAppendingString:@".bin"]] atomically:YES];
    }
    ((BOOL (*)(id, SEL, unsigned int, NSError **))objc_msgSend)(model, @selector(unloadWithQoS:error:), 21, nil);
  done:
    [fm removeItemAtPath:staging error:nil];
    NSData *json = [NSJSONSerialization dataWithJSONObject:@{@"rc": @(rc), @"identifier": ident,
        @"compile_ms": @(compileMs), @"load_ms": @(loadMs), @"exec_ms": times} options:0 error:nil];
    printf("%s\n", [[NSString alloc] initWithData:json encoding:NSUTF8StringEncoding].UTF8String);
    return rc;
  }
}
