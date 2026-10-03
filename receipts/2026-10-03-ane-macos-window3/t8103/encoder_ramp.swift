import Foundation
import CoreML

// Cold whole-encoder ramp on macOS (T8103 E1a): the window-4 encoder_bench with EVERY call timed, call 1
// included, so the DVFS climb after the first prediction is visible. Output: one JSON line with the
// same keys the ANE runner prints (exec_ms per call, compile_ms, load_ms, placement), then times_ms.
// usage: encoder_ramp <model.mlpackage> <feat_f32.bin> <mask_i32.bin> <units: all|ane|cpu> <calls> <dump_out.bin>
// Build: xcrun swiftc -O -target arm64-apple-macosx14.4 -o encoder_ramp encoder_ramp.swift  (MLComputePlan needs 14.4)

func die(_ m: String) -> Never { FileHandle.standardError.write((m + "\n").data(using: .utf8)!); exit(1) }

let a = CommandLine.arguments
guard a.count == 7 else { die("args: model feat mask units calls dump") }
let modelURL = URL(fileURLWithPath: a[1])
let unitsStr = a[4]
let calls = Int(a[5])!
let dumpPath = a[6]

let units: MLComputeUnits
switch unitsStr {
case "all": units = .all
case "ane": units = .cpuAndNeuralEngine
case "cpu": units = .cpuOnly
default: die("bad units")
}

let t0 = Date()
let compiledURL = try MLModel.compileModel(at: modelURL)
let dest = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("t8103-ramp-\(unitsStr)-\(UUID().uuidString).mlmodelc")
try FileManager.default.moveItem(at: compiledURL, to: dest)
let compileMs = -t0.timeIntervalSinceNow * 1000

let cfg = MLModelConfiguration()
cfg.computeUnits = units
let tl = Date()
let model = try MLModel(contentsOf: dest, configuration: cfg)
let loadMs = -tl.timeIntervalSinceNow * 1000

var placement: [String: Int] = [:]
let sem = DispatchSemaphore(value: 0)
Task.detached {
    defer { sem.signal() }
    guard let plan = try? await MLComputePlan.load(contentsOf: dest, configuration: cfg),
          case let .program(prog) = plan.modelStructure,
          let main = prog.functions["main"] else { return }
    for op in main.block.operations {
        guard let u = plan.deviceUsage(for: op)?.preferred else { continue }
        let k: String
        switch u {
        case .neuralEngine: k = "ane"
        case .gpu: k = "gpu"
        case .cpu: k = "cpu"
        @unknown default: k = "other"
        }
        placement[k, default: 0] += 1
    }
}
sem.wait()

let featData = try Data(contentsOf: URL(fileURLWithPath: a[2]))
let maskData = try Data(contentsOf: URL(fileURLWithPath: a[3]))
let featCount = 1 * 3000 * 128
let featArr = try MLMultiArray(shape: [1, 3000, 128] as [NSNumber], dataType: .float32)
let maskArr = try MLMultiArray(shape: [1, 3000] as [NSNumber], dataType: .int32)
memcpy(featArr.dataPointer, (featData as NSData).bytes, featCount * 4)
memcpy(maskArr.dataPointer, (maskData as NSData).bytes, 3000 * 4)
let featDesc = model.modelDescription
let inpName = featDesc.inputDescriptionsByName.keys.sorted().first { $0.contains("features") }!
let maskName = featDesc.inputDescriptionsByName.keys.sorted().first { $0.contains("mask") }!
let provider = try MLDictionaryFeatureProvider(dictionary: [inpName: MLFeatureValue(multiArray: featArr), maskName: MLFeatureValue(multiArray: maskArr)])

// no warm-up: call 1 is the cold start
var times: [Double] = []
var hidden: MLMultiArray? = nil
let wall0 = Date().timeIntervalSince1970
for _ in 0..<calls {
    let s = Date()
    let out = try model.prediction(from: provider)
    times.append(-s.timeIntervalSinceNow * 1000)
    hidden = out.featureValue(for: "encoder_hidden")!.multiArrayValue!
}
let wall1 = Date().timeIntervalSince1970

if let h = hidden {
    try Data(bytes: h.dataPointer, count: h.count * MemoryLayout<Float>.size).write(to: URL(fileURLWithPath: dumpPath))
}
let pl = placement.sorted { $0.key < $1.key }.map { "\"\($0.key)\":\($0.value)" }.joined(separator: ",")
let ex = times.map { String(format: "%.3f", $0) }.joined(separator: ",")
print("{\"units\":\"\(unitsStr)\",\"compile_ms\":\(String(format: "%.1f", compileMs)),\"load_ms\":\(String(format: "%.1f", loadMs)),\"placement\":{\(pl)},\"calls\":\(calls),\"wall_start_s\":\(String(format: "%.6f", wall0)),\"wall_end_s\":\(String(format: "%.6f", wall1)),\"exec_ms\":[\(ex)],\"rc\":0}")
print("times_ms [\(times.map { String(format: "%.2f", $0) }.joined(separator: ", "))]")
