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
  }
}
