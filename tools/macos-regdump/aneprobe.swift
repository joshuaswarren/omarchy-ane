/* Loop a tiny convolution on the Neural Engine so the ANE stays powered
 * while the register dump runs. Usage: aneprobe <model.mlmodelc> <seconds> */
import CoreML
import Foundation

let args = CommandLine.arguments
guard args.count == 3, let seconds = Int(args[2]) else {
    fputs("usage: aneprobe <model.mlmodelc> <seconds>\n", stderr)
    exit(2)
}
let cfg = MLModelConfiguration()
cfg.computeUnits = .cpuAndNeuralEngine
let model = try MLModel(contentsOf: URL(fileURLWithPath: args[1]), configuration: cfg)
let shape = [1, 3, 64, 64] as [NSNumber]
let arr = try MLMultiArray(shape: shape, dataType: .float32)
let input = try MLDictionaryFeatureProvider(dictionary: ["x": MLFeatureValue(multiArray: arr)])
let deadline = Date().addingTimeInterval(TimeInterval(seconds))
var n = 0
while Date() < deadline {
    _ = try model.prediction(from: input)
    n += 1
}
print("predictions=\(n)")
