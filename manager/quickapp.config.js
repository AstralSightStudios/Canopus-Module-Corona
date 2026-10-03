module.exports = {
  webpack: {
    module: {
      rules: [
        {
          test: /\.ts$/,
          exclude: /node_modules/,
          loader: "builtin:swc-loader",
          options: {
            jsc: {
              parser: {syntax: "typescript"},
              target: "es2015"
            }
          }
        }
      ]
    }
  },
  postHook(config) {
    if (config.mode !== "development") return
    // Large unminified page bundles fail during evaluation on the Vela emulator.
    // Keep development logs/source maps and avoid semantic compression passes.
    const Minimizer = config.optimization.minimizer[0].constructor
    config.optimization.minimizer = [new Minimizer({
      minimizerOptions: {
        module: true,
        minify: true,
        mangle: true,
        compress: { defaults: false }
      }
    })]
  }
}
