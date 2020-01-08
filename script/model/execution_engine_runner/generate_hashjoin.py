#!/usr/bin/env python3


def generate_build_key(col_num):
    # Generate the key type for the join
    print("struct BuildKey{} {{".format(col_num))
    for i in range(col_num):
        print("  c{} : Integer".format(i + 1))
    print("}")
    print()


def generate_build_row(col_num, agg_type):
    # Generate the join struct(key and value)
    print("struct BuildRow{} {{".format(col_num))
    print("  key: BuildKey{}".format(col_num))
    print("  agg: {}".format(agg_type))
    print("}")
    print()


def generate_key_check(col_num):
    print("fun keyCheck{}(execCtx: *ExecutionContext, pci: *ProjectedColumnsIterator, row: *BuildRow{}) -> bool {{"
          "".format(col_num, col_num))
    print("  return @sqlToBool(@pciGetInt(pci, {}) == row.key.c1)".format(col_num - 1), end="")
    for i in range(1, col_num):
        print(" and @sqlToBool(@pciGetInt(pci, {}) == row.key.c{})".format(col_num - 1, i + 1), end="")
    print("\n}\n")


def generate_build_side(col_num, row_num, cardinality):
    fun_name = "buildCol{}Row{}Car{}".format(col_num, row_num, cardinality)
    print("fun {}(execCtx: *ExecutionContext, state: *State) -> nil {{".format(fun_name))
    print("  @execCtxStartResourceTracker(execCtx)")

    print("  var jht: *JoinHashTable = &state.table{}".format(col_num))  # join hash table

    # table iterator
    print("  var tvi: TableVectorIterator")
    print("  var col_oids : [{}]uint32".format(col_num))
    for i in range(0, col_num):
        print("  col_oids[{}] = {}".format(i, 5 - i))

    print("  @tableIterInitBind(&tvi, execCtx, \"IntegerCol5Row{}Car{}\", col_oids)".format(row_num, cardinality))

    print("  for (@tableIterAdvance(&tvi)) {")
    print("    var vec = @tableIterGetPCI(&tvi)")
    print("    for (; @pciHasNext(vec); @pciAdvance(vec)) {")

    # calculate the join key
    print("      var hash_val = @hash(@pciGetInt(vec, {})".format(col_num - 1), end="")
    for i in range(1, col_num):
        print(", @pciGetInt(vec, {})".format(col_num - 1), end="")
    print(")")
    # insert into the join table
    print("      var elem : *BuildRow{} = @ptrCast(*BuildRow{}, @joinHTInsert(jht, hash_val))".format(col_num, col_num))

    # fill in the join key
    print("      elem.key.c1 = @pciGetInt(vec, {})".format(col_num - 1))
    for i in range(1, col_num):
        print("      elem.key.c{} = @pciGetInt(vec, {})".format(i + 1, col_num - 1))

    print("    }")
    print("  }")
    print("  @tableIterClose(&tvi)")

    # Build the join table
    print("  @joinHTBuild(jht)")

    print("  @execCtxEndResourceTracker(execCtx, @stringToSql(\"joinbuild, {}, {}, {}\"))".format(row_num, col_num * 4,
                                                                                                  cardinality))
    print("}")

    print()

    return fun_name


def generate_probe_side(col_num, row_num, cardinality):
    fun_name = "probeCol{}Row{}Car{}".format(col_num, row_num, cardinality)
    print("fun {}(execCtx: *ExecutionContext, state: *State) -> nil {{".format(fun_name))
    print("  @execCtxStartResourceTracker(execCtx)")

    print("  var jht: *JoinHashTable = &state.table{}".format(col_num))  # join hash table
    print("  var build_row: *BuildRow{}".format(col_num))

    # table iterator
    print("  var tvi: TableVectorIterator")
    print("  var col_oids : [{}]uint32".format(col_num))
    for i in range(0, col_num):
        print("  col_oids[{}] = {}".format(i, 5 - i))
    print("  @tableIterInitBind(&tvi, execCtx, \"IntegerCol5Row{}Car{}\", col_oids)".format(row_num, cardinality))

    print("  for (@tableIterAdvance(&tvi)) {")
    print("    var vec = @tableIterGetPCI(&tvi)")
    print("    for (; @pciHasNext(vec); @pciAdvance(vec)) {")

    # calculate the join key
    print("      var hash_val = @hash(@pciGetInt(vec, {})".format(col_num - 1), end="")
    for i in range(1, col_num):
        print(", @pciGetInt(vec, {})".format(col_num - 1), end="")
    print(")")

    # iterate the hash table
    print("      var hti: JoinHashTableIterator")
    print("      for (@joinHTIterInit(&hti, jht, hash_val); @joinHTIterHasNext(&hti, keyCheck{}, execCtx, "
          "vec);) {{".format(col_num))
    print("        build_row = @ptrCast(*BuildRow{}, @joinHTIterGetRow(&hti))".format(col_num))
    print("        state.num_matches = state.num_matches + 1")
    print("      }")

    print("    }")
    print("  }")
    print("  @tableIterClose(&tvi)")

    # Build the join table
    print("  @joinHTBuild(jht)")

    print("  @execCtxEndResourceTracker(execCtx, @stringToSql(\"joinprobe, {}, {}, {}\"))".format(row_num, col_num * 4,
                                                                                                  cardinality))
    print("}")

    print()

    return fun_name


def generate_state(col_nums):
    print("struct State {")
    for i in col_nums:
        print("  table{} : JoinHashTable".format(i))
    print("  num_matches : int64")
    print("}\n")


def generate_tear_down(col_nums):
    print("fun tearDownState(execCtx: *ExecutionContext, state: *State) -> nil {")
    for i in col_nums:
        print("  @joinHTFree(&state.table{})".format(i))
    print("}\n")


def generate_setup(col_nums):
    print("fun setUpState(execCtx: *ExecutionContext, state: *State) -> nil {")
    for i in col_nums:
        print("  @joinHTInit(&state.table{}, @execCtxGetMem(execCtx), @sizeOf(BuildRow{}))".format(i, i))

    print("  state.num_matches = 0")
    print("}\n")


def generate_main_fun(fun_names):
    print("fun main(execCtx: *ExecutionContext) -> int32 {")
    print("  var state: State")

    for fun_name in fun_names:
        if "build" in fun_name:
            print("\n  setUpState(execCtx, &state)")
        print("  {}(execCtx, &state)".format(fun_name))

        if "probe" in fun_name:
            print("  tearDownState(execCtx, &state)")

    print("\n  return state.num_matches")
    print("}")


def generate_all():
    agg_types = ['IntegerSumAggregate', 'CountStarAggregate', 'IntegerAvgAggregate', 'IntegerMinAggregate',
                 'IntegerMaxAggregate']
    fun_names = []
    col_nums = range(1, 6)
    row_nums = [1, 5, 10, 50, 100, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000,
                200000, 500000, 1000000]
    cardinalities = [1, 2, 5, 10, 50, 100]

    for col_num in col_nums:
        generate_build_key(col_num)

    for col_num in col_nums:
        generate_build_row(col_num, agg_types[col_num - 1])

    generate_state(col_nums)
    generate_setup(col_nums)
    generate_tear_down(col_nums)

    for col_num in col_nums:
        generate_key_check(col_num)

    for col_num in col_nums:
        for row_num in row_nums:
            for cardinality in cardinalities:
                fun_names.append(generate_build_side(col_num, row_num, cardinality))
                fun_names.append(generate_probe_side(col_num, row_num, cardinality))

    generate_main_fun(fun_names)


if __name__ == '__main__':
    generate_all()