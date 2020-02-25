def _get_real_short_path(file):
    # For some reason, files from other archives have short paths that look like:
    #   ../com_google_protobuf/google/protobuf/descriptor.proto
    short_path = file.short_path
    if short_path.startswith("../"):
        second_slash = short_path.index("/", 3)
        short_path = short_path[second_slash + 1:]
    return short_path

def _protos_impl(ctx):
    """Rule to copy all protos to a single output directory."""
    proto_files = []
    for dep in ctx.attr.deps:
        proto_files.extend(dep[ProtoInfo].transitive_sources.to_list())

    outputs = []
    for f in proto_files:
        short_path = _get_real_short_path(f)
        out = ctx.actions.declare_file(short_path)
        outputs.append(out)
        ctx.actions.run_shell(
            outputs = [out],
            inputs = depset([f]),
            arguments = [f.path, out.path],
            command = "cp $1 $2",
        )
    return [
        DefaultInfo(
            files = depset(outputs),
            data_runfiles = ctx.runfiles(files = outputs),
        ),
    ]

protos = rule(
    implementation = _protos_impl,
    attrs = {
        "deps": attr.label_list(providers = [ProtoInfo]),
    },
)
