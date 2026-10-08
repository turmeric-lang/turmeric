class Turmeric < Formula
  desc "A Lisp that compiles to C99 with typeclasses, algebraic effects, and borrow checking"
  homepage "https://turmeric-lang.com"
  license "MIT"
  head "https://github.com/turmeric-lang/turmeric.git", branch: "main"

  depends_on "cmake" => :build

  def install
    system "cmake", "-S", ".", "-B", "build",
                    "-DCMAKE_BUILD_TYPE=Release",
                    "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
                    *std_cmake_args
    system "cmake", "--build", "build", "-j"
    bin.install "build/tur"
    (share/"turmeric").install "stdlib"

    # OD4: install the rendered guides and API reference when the checkout has
    # them, so `tur docs --open` finds a local copy. A fresh clone does not --
    # docs/html/ is a `just docs` output and is gitignored -- and generating it
    # here would mean depending on python3 + the markdown package for something
    # that is optional to running the compiler. So this is a no-op for the
    # common case, and the release's turmeric-docs-<version>.tar.gz is the
    # supported way to get them: unpack it and point TUR_DOCS_DIR at the
    # directory holding guides/ and api/. `tur doc <symbol>` needs none of
    # this; its table ships in the stdlib installed just above.
    doc.install Dir["docs/html/*"] if File.directory?("docs/html/guides")

    # Runs `tur completion zsh` / `tur completion bash` -- the default
    # shell_parameter_format passes the shell name as a positional argument,
    # which is exactly the CLI shape.
    generate_completions_from_executable(bin/"tur", "completion",
                                         shells: [:zsh, :bash])
  end

  test do
    # Exercises stdlib resolution: `when` is defined in stdlib/macros.tur,
    # so if the formula didn't install stdlib under <prefix>/share/turmeric/
    # this test fails with "unbound variable: when". Uses --interpret to skip
    # the C-codegen path, which currently still depends on src/runtime/*.c
    # being reachable (tracked separately in docs/release-binaries-plan.md).
    (testpath/"hello.tur").write <<~TUR
      (defn main [] :int
        (when (= 1 1) (println "hello from stdlib"))
        0)
    TUR
    assert_match "hello from stdlib",
                 shell_output("#{bin}/tur --interpret #{testpath}/hello.tur")
  end
end
