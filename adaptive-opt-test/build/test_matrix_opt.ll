; ModuleID = 'build/test_matrix_adaptive.ll'
source_filename = "test-programs/test_matrix.cpp"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

module asm ".globl _ZSt21ios_base_library_initv"

%"class.std::basic_ostream" = type { ptr, %"class.std::basic_ios" }
%"class.std::basic_ios" = type { %"class.std::ios_base", ptr, i8, i8, ptr, ptr, ptr, ptr }
%"class.std::ios_base" = type { ptr, i64, i64, i32, i32, i32, ptr, %"struct.std::ios_base::_Words", [8 x %"struct.std::ios_base::_Words"], i32, ptr, %"class.std::locale" }
%"struct.std::ios_base::_Words" = type { ptr, i64 }
%"class.std::locale" = type { ptr }
%"class.std::chrono::time_point" = type { %"class.std::chrono::duration" }
%"class.std::chrono::duration" = type { i64 }
%"class.std::chrono::duration.0" = type { i64 }

$_ZNSt6chrono13duration_castINS_8durationIlSt5ratioILl1ELl1000000EEEElS2_ILl1ELl1000000000EEEENSt9enable_ifIXsr13__is_durationIT_EE5valueES7_E4typeERKNS1_IT0_T1_EE = comdat any

$_ZNSt6chronomiINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEES6_EENSt11common_typeIJT0_T1_EE4typeERKNS_10time_pointIT_S8_EERKNSC_ISD_S9_EE = comdat any

$_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000EEE5countEv = comdat any

$_ZNSt6chrono20__duration_cast_implINS_8durationIlSt5ratioILl1ELl1000000EEEES2_ILl1ELl1000EElLb1ELb0EE6__castIlS2_ILl1ELl1000000000EEEES4_RKNS1_IT_T0_EE = comdat any

$_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000000EEE5countEv = comdat any

$_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000EEEC2IlvEERKT_ = comdat any

$_ZNSt6chronomiIlSt5ratioILl1ELl1000000000EElS2_EENSt11common_typeIJNS_8durationIT_T0_EENS4_IT1_T2_EEEE4typeERKS7_RKSA_ = comdat any

$_ZNKSt6chrono10time_pointINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEEE16time_since_epochEv = comdat any

$_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEC2IlvEERKT_ = comdat any

@_ZSt4cout = external global %"class.std::basic_ostream", align 8
@.str = private unnamed_addr constant [31 x i8] c"Testing Adaptive Optimization\0A\00", align 1
@.str.1 = private unnamed_addr constant [32 x i8] c"=============================\0A\0A\00", align 1
@.str.2 = private unnamed_addr constant [20 x i8] c"Call 1 (using v0): \00", align 1
@.str.3 = private unnamed_addr constant [6 x i8] c" \C2\B5s\0A\00", align 1
@.str.4 = private unnamed_addr constant [21 x i8] c"Call 100 (last v0): \00", align 1
@.str.5 = private unnamed_addr constant [22 x i8] c"Call 101 (first v1): \00", align 1
@.str.6 = private unnamed_addr constant [22 x i8] c"Call 200 (using v1): \00", align 1
@.str.7 = private unnamed_addr constant [27 x i8] c"\0AResult sample: C[0][0] = \00", align 1
@.str.8 = private unnamed_addr constant [2 x i8] c"\0A\00", align 1
@call_count_v0 = internal unnamed_addr global i32 0
@call_count_v1 = internal unnamed_addr global i32 0
@total_cycles_v0 = internal unnamed_addr global i64 0
@total_cycles_v1 = internal unnamed_addr global i64 0
@0 = private unnamed_addr constant [37 x i8] c"BEST VERSION: %s (Avg: %lld cycles)\0A\00", align 1
@1 = private unnamed_addr constant [27 x i8] c"DISPATCH: V0 (Baseline) - \00", align 1
@2 = private unnamed_addr constant [28 x i8] c"DISPATCH: V1 (Optimized) - \00", align 1
@3 = private unnamed_addr constant [38 x i8] c"Cycles: %lld, Total: %lld, Count: %d\0A\00", align 1
@4 = private unnamed_addr constant [55 x i8] c"V0 (Baseline): avg=%lld cycles (total=%lld, count=%d)\0A\00", align 1
@5 = private unnamed_addr constant [56 x i8] c"V1 (Optimized): avg=%lld cycles (total=%lld, count=%d)\0A\00", align 1
@6 = private unnamed_addr constant [15 x i8] c"V1 (Optimized)\00", align 1
@7 = private unnamed_addr constant [14 x i8] c"V0 (Baseline)\00", align 1
@8 = private unnamed_addr constant [4 x i8] c"N/A\00", align 1
@9 = private unnamed_addr constant [18 x i8] c"Best Version: %s\0A\00", align 1
@str = private unnamed_addr constant [29 x i8] c"\0A=== Performance Summary ===\00", align 1
@str.1 = private unnamed_addr constant [28 x i8] c"===========================\00", align 1

; Function Attrs: mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare float @llvm.fmuladd.f32(float, float, float) #0

; Function Attrs: noinline norecurse uwtable
define dso_local noundef i32 @main() local_unnamed_addr #1 {
vector.ph:
  %0 = alloca %"class.std::chrono::time_point", align 8
  %1 = alloca %"class.std::chrono::time_point", align 8
  %2 = alloca %"class.std::chrono::duration.0", align 8
  %3 = alloca %"class.std::chrono::duration", align 8
  %4 = tail call noalias noundef nonnull dereferenceable(10000) ptr @_Znam(i64 noundef 10000) #15
  %5 = tail call noalias noundef nonnull dereferenceable(10000) ptr @_Znam(i64 noundef 10000) #15
  %6 = tail call noalias noundef nonnull dereferenceable(10000) ptr @_Znam(i64 noundef 10000) #15
  br label %vector.body

vector.body:                                      ; preds = %vector.body, %vector.ph
  %index = phi i64 [ 0, %vector.ph ], [ %index.next, %vector.body ]
  %vec.ind = phi <4 x i32> [ <i32 0, i32 1, i32 2, i32 3>, %vector.ph ], [ %vec.ind.next, %vector.body ]
  %step.add = add <4 x i32> %vec.ind, <i32 4, i32 4, i32 4, i32 4>
  %7 = sitofp <4 x i32> %vec.ind to <4 x float>
  %8 = sitofp <4 x i32> %step.add to <4 x float>
  %9 = fmul <4 x float> %7, <float 0x3F847AE140000000, float 0x3F847AE140000000, float 0x3F847AE140000000, float 0x3F847AE140000000>
  %10 = fmul <4 x float> %8, <float 0x3F847AE140000000, float 0x3F847AE140000000, float 0x3F847AE140000000, float 0x3F847AE140000000>
  %11 = getelementptr inbounds float, ptr %4, i64 %index
  %12 = getelementptr inbounds float, ptr %11, i64 4
  store <4 x float> %9, ptr %11, align 4
  store <4 x float> %10, ptr %12, align 4
  %13 = fmul <4 x float> %7, <float 0x3F947AE140000000, float 0x3F947AE140000000, float 0x3F947AE140000000, float 0x3F947AE140000000>
  %14 = fmul <4 x float> %8, <float 0x3F947AE140000000, float 0x3F947AE140000000, float 0x3F947AE140000000, float 0x3F947AE140000000>
  %15 = getelementptr inbounds float, ptr %5, i64 %index
  %16 = getelementptr inbounds float, ptr %15, i64 4
  store <4 x float> %13, ptr %15, align 4
  store <4 x float> %14, ptr %16, align 4
  %index.next = add nuw i64 %index, 8
  %vec.ind.next = add <4 x i32> %vec.ind, <i32 8, i32 8, i32 8, i32 8>
  %17 = icmp eq i64 %index.next, 2496
  br i1 %17, label %scalar.ph, label %vector.body, !llvm.loop !6

scalar.ph:                                        ; preds = %vector.body
  %18 = getelementptr inbounds float, ptr %4, i64 2496
  store float 0x4038F5C280000000, ptr %18, align 4
  %19 = getelementptr inbounds float, ptr %5, i64 2496
  store float 0x4048F5C280000000, ptr %19, align 4
  %20 = getelementptr inbounds float, ptr %4, i64 2497
  store float 0x4038F851E0000000, ptr %20, align 4
  %21 = getelementptr inbounds float, ptr %5, i64 2497
  store float 0x4048F851E0000000, ptr %21, align 4
  %22 = getelementptr inbounds float, ptr %4, i64 2498
  store float 0x4038FAE140000000, ptr %22, align 4
  %23 = getelementptr inbounds float, ptr %5, i64 2498
  store float 0x4048FAE140000000, ptr %23, align 4
  %24 = getelementptr inbounds float, ptr %4, i64 2499
  store float 0x4038FD70A0000000, ptr %24, align 4
  %25 = getelementptr inbounds float, ptr %5, i64 2499
  store float 0x4048FD70A0000000, ptr %25, align 4
  %26 = tail call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) @_ZSt4cout, ptr noundef nonnull @.str)
  %27 = tail call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) @_ZSt4cout, ptr noundef nonnull @.str.1)
  br label %28

28:                                               ; preds = %scalar.ph, %40
  %.023 = phi i32 [ 0, %scalar.ph ], [ %41, %40 ]
  call void @llvm.memset.p0.i64(ptr noundef nonnull align 4 dereferenceable(10000) %6, i8 0, i64 10000, i1 false)
  %29 = call i64 @_ZNSt6chrono3_V212system_clock3nowEv() #13
  store i64 %29, ptr %0, align 8
  call void @matrix_multiply(ptr noundef nonnull %4, ptr noundef nonnull %5, ptr noundef nonnull %6, i32 noundef 50)
  %30 = call i64 @_ZNSt6chrono3_V212system_clock3nowEv() #13
  store i64 %30, ptr %1, align 8
  %31 = call i64 @_ZNSt6chronomiINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEES6_EENSt11common_typeIJT0_T1_EE4typeERKNS_10time_pointIT_S8_EERKNSC_ISD_S9_EE(ptr noundef nonnull align 8 dereferenceable(8) %1, ptr noundef nonnull align 8 dereferenceable(8) %0)
  store i64 %31, ptr %3, align 8
  %32 = call i64 @_ZNSt6chrono13duration_castINS_8durationIlSt5ratioILl1ELl1000000EEEElS2_ILl1ELl1000000000EEEENSt9enable_ifIXsr13__is_durationIT_EE5valueES7_E4typeERKNS1_IT0_T1_EE(ptr noundef nonnull align 8 dereferenceable(8) %3)
  store i64 %32, ptr %2, align 8
  %trunc = trunc i32 %.023 to i8
  switch i8 %trunc, label %40 [
    i8 0, label %.sink.split
    i8 99, label %33
    i8 100, label %34
    i8 -57, label %35
  ]

33:                                               ; preds = %28
  br label %.sink.split

34:                                               ; preds = %28
  br label %.sink.split

35:                                               ; preds = %28
  br label %.sink.split

.sink.split:                                      ; preds = %28, %33, %35, %34
  %.str.2.sink = phi ptr [ @.str.5, %34 ], [ @.str.6, %35 ], [ @.str.4, %33 ], [ @.str.2, %28 ]
  %36 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) @_ZSt4cout, ptr noundef nonnull %.str.2.sink)
  %37 = call noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %2)
  %38 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZNSolsEl(ptr noundef nonnull align 8 dereferenceable(8) %36, i64 noundef %37)
  %39 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) %38, ptr noundef nonnull @.str.3)
  br label %40

40:                                               ; preds = %.sink.split, %28
  %41 = add nuw nsw i32 %.023, 1
  %exitcond25.not = icmp eq i32 %41, 200
  br i1 %exitcond25.not, label %42, label %28, !llvm.loop !10

42:                                               ; preds = %40
  %43 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) @_ZSt4cout, ptr noundef nonnull @.str.7)
  %44 = load float, ptr %6, align 4
  %45 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZNSolsEf(ptr noundef nonnull align 8 dereferenceable(8) %43, float noundef %44)
  %46 = call noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8) %45, ptr noundef nonnull @.str.8)
  %puts.i = call i32 @puts(ptr nonnull dereferenceable(1) @str)
  %47 = load i32, ptr @call_count_v0, align 4
  %48 = load i32, ptr @call_count_v1, align 4
  %49 = load i64, ptr @total_cycles_v0, align 8
  %50 = load i64, ptr @total_cycles_v1, align 8
  %.not.i = icmp eq i32 %47, 0
  %.not1.i = icmp eq i32 %48, 0
  %51 = zext i32 %47 to i64
  %52 = zext i32 %48 to i64
  %53 = udiv i64 %49, %51
  %54 = udiv i64 %50, %52
  %55 = select i1 %.not.i, i64 0, i64 %53
  %56 = select i1 %.not1.i, i64 0, i64 %54
  %57 = call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @4, i64 %55, i64 %49, i32 %47)
  %58 = call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @5, i64 %56, i64 %50, i32 %48)
  %59 = icmp ult i64 %56, %55
  %60 = select i1 %.not.i, ptr @8, ptr @7
  %61 = select i1 %.not.i, i1 true, i1 %59
  %62 = select i1 %61, ptr @6, ptr @7
  %63 = select i1 %.not1.i, ptr %60, ptr %62
  %64 = call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @9, ptr nonnull %63)
  %puts2.i = call i32 @puts(ptr nonnull dereferenceable(1) @str.1)
  call void @_ZdaPv(ptr noundef nonnull %4) #16
  call void @_ZdaPv(ptr noundef nonnull %5) #16
  call void @_ZdaPv(ptr noundef nonnull %6) #16
  ret i32 0
}

; Function Attrs: nobuiltin allocsize(0)
declare noundef nonnull ptr @_Znam(i64 noundef) local_unnamed_addr #2

declare noundef nonnull align 8 dereferenceable(8) ptr @_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc(ptr noundef nonnull align 8 dereferenceable(8), ptr noundef) local_unnamed_addr #3

; Function Attrs: mustprogress nocallback nofree nounwind willreturn memory(argmem: write)
declare void @llvm.memset.p0.i64(ptr nocapture writeonly, i8, i64, i1 immarg) #4

; Function Attrs: nounwind
declare i64 @_ZNSt6chrono3_V212system_clock3nowEv() local_unnamed_addr #5

; Function Attrs: mustprogress noinline uwtable
define linkonce_odr dso_local i64 @_ZNSt6chrono13duration_castINS_8durationIlSt5ratioILl1ELl1000000EEEElS2_ILl1ELl1000000000EEEENSt9enable_ifIXsr13__is_durationIT_EE5valueES7_E4typeERKNS1_IT0_T1_EE(ptr noundef nonnull align 8 dereferenceable(8) %0) local_unnamed_addr #6 comdat {
  %2 = tail call i64 @_ZNSt6chrono20__duration_cast_implINS_8durationIlSt5ratioILl1ELl1000000EEEES2_ILl1ELl1000EElLb1ELb0EE6__castIlS2_ILl1ELl1000000000EEEES4_RKNS1_IT_T0_EE(ptr noundef nonnull align 8 dereferenceable(8) %0)
  ret i64 %2
}

; Function Attrs: mustprogress noinline uwtable
define linkonce_odr dso_local i64 @_ZNSt6chronomiINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEES6_EENSt11common_typeIJT0_T1_EE4typeERKNS_10time_pointIT_S8_EERKNSC_ISD_S9_EE(ptr noundef nonnull align 8 dereferenceable(8) %0, ptr noundef nonnull align 8 dereferenceable(8) %1) local_unnamed_addr #6 comdat {
  %3 = alloca %"class.std::chrono::duration", align 8
  %4 = alloca %"class.std::chrono::duration", align 8
  %5 = tail call i64 @_ZNKSt6chrono10time_pointINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEEE16time_since_epochEv(ptr noundef nonnull align 8 dereferenceable(8) %0)
  store i64 %5, ptr %3, align 8
  %6 = tail call i64 @_ZNKSt6chrono10time_pointINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEEE16time_since_epochEv(ptr noundef nonnull align 8 dereferenceable(8) %1)
  store i64 %6, ptr %4, align 8
  %7 = call i64 @_ZNSt6chronomiIlSt5ratioILl1ELl1000000000EElS2_EENSt11common_typeIJNS_8durationIT_T0_EENS4_IT1_T2_EEEE4typeERKS7_RKSA_(ptr noundef nonnull align 8 dereferenceable(8) %3, ptr noundef nonnull align 8 dereferenceable(8) %4)
  ret i64 %7
}

declare noundef nonnull align 8 dereferenceable(8) ptr @_ZNSolsEl(ptr noundef nonnull align 8 dereferenceable(8), i64 noundef) local_unnamed_addr #3

; Function Attrs: mustprogress noinline nounwind uwtable
define linkonce_odr dso_local noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %0) local_unnamed_addr #7 comdat align 2 {
  %2 = load i64, ptr %0, align 8
  ret i64 %2
}

declare noundef nonnull align 8 dereferenceable(8) ptr @_ZNSolsEf(ptr noundef nonnull align 8 dereferenceable(8), float noundef) local_unnamed_addr #3

; Function Attrs: nofree nounwind
define void @print_performance_summary() local_unnamed_addr #8 {
entry:
  %puts = tail call i32 @puts(ptr nonnull dereferenceable(1) @str)
  %0 = load i32, ptr @call_count_v0, align 4
  %1 = load i32, ptr @call_count_v1, align 4
  %2 = load i64, ptr @total_cycles_v0, align 8
  %3 = load i64, ptr @total_cycles_v1, align 8
  %.not = icmp eq i32 %0, 0
  %.not1 = icmp eq i32 %1, 0
  %4 = zext i32 %0 to i64
  %5 = zext i32 %1 to i64
  %6 = udiv i64 %2, %4
  %7 = udiv i64 %3, %5
  %8 = select i1 %.not, i64 0, i64 %6
  %9 = select i1 %.not1, i64 0, i64 %7
  %10 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @4, i64 %8, i64 %2, i32 %0)
  %11 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @5, i64 %9, i64 %3, i32 %1)
  %12 = icmp ult i64 %9, %8
  %13 = select i1 %.not, ptr @8, ptr @7
  %14 = select i1 %.not, i1 true, i1 %12
  %15 = select i1 %14, ptr @6, ptr @7
  %16 = select i1 %.not1, ptr %13, ptr %15
  %17 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @9, ptr nonnull %16)
  %puts2 = tail call i32 @puts(ptr nonnull dereferenceable(1) @str.1)
  ret void
}

; Function Attrs: nobuiltin nounwind
declare void @_ZdaPv(ptr noundef) local_unnamed_addr #9

; Function Attrs: mustprogress noinline uwtable
define linkonce_odr dso_local i64 @_ZNSt6chrono20__duration_cast_implINS_8durationIlSt5ratioILl1ELl1000000EEEES2_ILl1ELl1000EElLb1ELb0EE6__castIlS2_ILl1ELl1000000000EEEES4_RKNS1_IT_T0_EE(ptr noundef nonnull align 8 dereferenceable(8) %0) local_unnamed_addr #6 comdat align 2 {
  %2 = alloca %"class.std::chrono::duration.0", align 8
  %3 = alloca i64, align 8
  %4 = tail call noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %0)
  %5 = sdiv i64 %4, 1000
  store i64 %5, ptr %3, align 8
  call void @_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000EEEC2IlvEERKT_(ptr noundef nonnull align 8 dereferenceable(8) %2, ptr noundef nonnull align 8 dereferenceable(8) %3)
  %6 = load i64, ptr %2, align 8
  ret i64 %6
}

; Function Attrs: mustprogress noinline nounwind uwtable
define linkonce_odr dso_local noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %0) local_unnamed_addr #7 comdat align 2 {
  %2 = load i64, ptr %0, align 8
  ret i64 %2
}

; Function Attrs: mustprogress noinline nounwind uwtable
define linkonce_odr dso_local void @_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000EEEC2IlvEERKT_(ptr noundef nonnull align 8 dereferenceable(8) %0, ptr noundef nonnull align 8 dereferenceable(8) %1) unnamed_addr #7 comdat align 2 {
  %3 = load i64, ptr %1, align 8
  store i64 %3, ptr %0, align 8
  ret void
}

; Function Attrs: mustprogress noinline uwtable
define linkonce_odr dso_local i64 @_ZNSt6chronomiIlSt5ratioILl1ELl1000000000EElS2_EENSt11common_typeIJNS_8durationIT_T0_EENS4_IT1_T2_EEEE4typeERKS7_RKSA_(ptr noundef nonnull align 8 dereferenceable(8) %0, ptr noundef nonnull align 8 dereferenceable(8) %1) local_unnamed_addr #6 comdat {
  %3 = alloca %"class.std::chrono::duration", align 8
  %4 = alloca i64, align 8
  %5 = alloca %"class.std::chrono::duration", align 8
  %6 = alloca %"class.std::chrono::duration", align 8
  %7 = load i64, ptr %0, align 8
  store i64 %7, ptr %5, align 8
  %8 = call noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %5)
  %9 = load i64, ptr %1, align 8
  store i64 %9, ptr %6, align 8
  %10 = call noundef i64 @_ZNKSt6chrono8durationIlSt5ratioILl1ELl1000000000EEE5countEv(ptr noundef nonnull align 8 dereferenceable(8) %6)
  %11 = sub nsw i64 %8, %10
  store i64 %11, ptr %4, align 8
  call void @_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEC2IlvEERKT_(ptr noundef nonnull align 8 dereferenceable(8) %3, ptr noundef nonnull align 8 dereferenceable(8) %4)
  %12 = load i64, ptr %3, align 8
  ret i64 %12
}

; Function Attrs: mustprogress noinline nounwind uwtable
define linkonce_odr dso_local i64 @_ZNKSt6chrono10time_pointINS_3_V212system_clockENS_8durationIlSt5ratioILl1ELl1000000000EEEEE16time_since_epochEv(ptr noundef nonnull align 8 dereferenceable(8) %0) local_unnamed_addr #7 comdat align 2 {
  %.sroa.0.0.copyload = load i64, ptr %0, align 8
  ret i64 %.sroa.0.0.copyload
}

; Function Attrs: mustprogress noinline nounwind uwtable
define linkonce_odr dso_local void @_ZNSt6chrono8durationIlSt5ratioILl1ELl1000000000EEEC2IlvEERKT_(ptr noundef nonnull align 8 dereferenceable(8) %0, ptr noundef nonnull align 8 dereferenceable(8) %1) unnamed_addr #7 comdat align 2 {
  %3 = load i64, ptr %1, align 8
  store i64 %3, ptr %0, align 8
  ret void
}

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @matrix_multiply_v0.2(ptr nocapture noundef readonly %0, ptr nocapture noundef readonly %1, ptr nocapture noundef writeonly %2, i32 noundef %3) local_unnamed_addr #10 {
  %5 = icmp sgt i32 %3, 0
  br i1 %5, label %.preheader26.lr.ph, label %._crit_edge32

.preheader26.lr.ph:                               ; preds = %4
  %6 = zext nneg i32 %3 to i64
  %wide.trip.count42 = zext nneg i32 %3 to i64
  %xtraiter = and i64 %wide.trip.count42, 3
  %7 = icmp ult i32 %3, 4
  %unroll_iter = and i64 %wide.trip.count42, 2147483644
  %lcmp.mod.not = icmp eq i64 %xtraiter, 0
  br label %.preheader.lr.ph

.preheader.lr.ph:                                 ; preds = %._crit_edge30, %.preheader26.lr.ph
  %indvars.iv39 = phi i64 [ 0, %.preheader26.lr.ph ], [ %indvars.iv.next40, %._crit_edge30 ]
  %8 = mul nsw i64 %indvars.iv39, %6
  %9 = and i64 %8, 4294967295
  %invariant.gep = getelementptr float, ptr %0, i64 %9
  %invariant.gep46 = getelementptr float, ptr %2, i64 %8
  br label %.preheader

.preheader:                                       ; preds = %.preheader.lr.ph, %._crit_edge
  %indvars.iv34 = phi i64 [ 0, %.preheader.lr.ph ], [ %indvars.iv.next35, %._crit_edge ]
  %invariant.gep44 = getelementptr float, ptr %1, i64 %indvars.iv34
  br i1 %7, label %._crit_edge.unr-lcssa, label %.preheader.new

.preheader.new:                                   ; preds = %.preheader, %.preheader.new
  %indvars.iv = phi i64 [ %indvars.iv.next.3, %.preheader.new ], [ 0, %.preheader ]
  %.02227 = phi float [ %25, %.preheader.new ], [ 0.000000e+00, %.preheader ]
  %niter = phi i64 [ %niter.next.3, %.preheader.new ], [ 0, %.preheader ]
  %gep = getelementptr float, ptr %invariant.gep, i64 %indvars.iv
  %10 = load float, ptr %gep, align 4
  %11 = mul nsw i64 %indvars.iv, %6
  %gep45 = getelementptr float, ptr %invariant.gep44, i64 %11
  %12 = load float, ptr %gep45, align 4
  %13 = tail call float @llvm.fmuladd.f32(float %10, float %12, float %.02227)
  %indvars.iv.next = or disjoint i64 %indvars.iv, 1
  %gep.1 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next
  %14 = load float, ptr %gep.1, align 4
  %15 = mul nsw i64 %indvars.iv.next, %6
  %gep45.1 = getelementptr float, ptr %invariant.gep44, i64 %15
  %16 = load float, ptr %gep45.1, align 4
  %17 = tail call float @llvm.fmuladd.f32(float %14, float %16, float %13)
  %indvars.iv.next.1 = or disjoint i64 %indvars.iv, 2
  %gep.2 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next.1
  %18 = load float, ptr %gep.2, align 4
  %19 = mul nsw i64 %indvars.iv.next.1, %6
  %gep45.2 = getelementptr float, ptr %invariant.gep44, i64 %19
  %20 = load float, ptr %gep45.2, align 4
  %21 = tail call float @llvm.fmuladd.f32(float %18, float %20, float %17)
  %indvars.iv.next.2 = or disjoint i64 %indvars.iv, 3
  %gep.3 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next.2
  %22 = load float, ptr %gep.3, align 4
  %23 = mul nsw i64 %indvars.iv.next.2, %6
  %gep45.3 = getelementptr float, ptr %invariant.gep44, i64 %23
  %24 = load float, ptr %gep45.3, align 4
  %25 = tail call float @llvm.fmuladd.f32(float %22, float %24, float %21)
  %indvars.iv.next.3 = add nuw nsw i64 %indvars.iv, 4
  %niter.next.3 = add i64 %niter, 4
  %niter.ncmp.3 = icmp eq i64 %niter.next.3, %unroll_iter
  br i1 %niter.ncmp.3, label %._crit_edge.unr-lcssa, label %.preheader.new, !llvm.loop !11

._crit_edge.unr-lcssa:                            ; preds = %.preheader.new, %.preheader
  %.lcssa.ph = phi float [ undef, %.preheader ], [ %25, %.preheader.new ]
  %indvars.iv.unr = phi i64 [ 0, %.preheader ], [ %indvars.iv.next.3, %.preheader.new ]
  %.02227.unr = phi float [ 0.000000e+00, %.preheader ], [ %25, %.preheader.new ]
  br i1 %lcmp.mod.not, label %._crit_edge, label %.epil.preheader

.epil.preheader:                                  ; preds = %._crit_edge.unr-lcssa, %.epil.preheader
  %indvars.iv.epil = phi i64 [ %indvars.iv.next.epil, %.epil.preheader ], [ %indvars.iv.unr, %._crit_edge.unr-lcssa ]
  %.02227.epil = phi float [ %29, %.epil.preheader ], [ %.02227.unr, %._crit_edge.unr-lcssa ]
  %epil.iter = phi i64 [ %epil.iter.next, %.epil.preheader ], [ 0, %._crit_edge.unr-lcssa ]
  %gep.epil = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.epil
  %26 = load float, ptr %gep.epil, align 4
  %27 = mul nsw i64 %indvars.iv.epil, %6
  %gep45.epil = getelementptr float, ptr %invariant.gep44, i64 %27
  %28 = load float, ptr %gep45.epil, align 4
  %29 = tail call float @llvm.fmuladd.f32(float %26, float %28, float %.02227.epil)
  %indvars.iv.next.epil = add nuw nsw i64 %indvars.iv.epil, 1
  %epil.iter.next = add i64 %epil.iter, 1
  %epil.iter.cmp.not = icmp eq i64 %epil.iter.next, %xtraiter
  br i1 %epil.iter.cmp.not, label %._crit_edge, label %.epil.preheader, !llvm.loop !12

._crit_edge:                                      ; preds = %.epil.preheader, %._crit_edge.unr-lcssa
  %.lcssa = phi float [ %.lcssa.ph, %._crit_edge.unr-lcssa ], [ %29, %.epil.preheader ]
  %gep47 = getelementptr float, ptr %invariant.gep46, i64 %indvars.iv34
  store float %.lcssa, ptr %gep47, align 4
  %indvars.iv.next35 = add nuw nsw i64 %indvars.iv34, 1
  %exitcond38.not = icmp eq i64 %indvars.iv.next35, %wide.trip.count42
  br i1 %exitcond38.not, label %._crit_edge30, label %.preheader, !llvm.loop !14

._crit_edge30:                                    ; preds = %._crit_edge
  %indvars.iv.next40 = add nuw nsw i64 %indvars.iv39, 1
  %exitcond43.not = icmp eq i64 %indvars.iv.next40, %wide.trip.count42
  br i1 %exitcond43.not, label %._crit_edge32, label %.preheader.lr.ph, !llvm.loop !15

._crit_edge32:                                    ; preds = %._crit_edge30, %4
  ret void
}

; Function Attrs: mustprogress nofree noinline norecurse nosync nounwind memory(argmem: readwrite) uwtable
define dso_local void @matrix_multiply_v1.4(ptr nocapture noundef readonly %0, ptr nocapture noundef readonly %1, ptr nocapture noundef writeonly %2, i32 noundef %3) local_unnamed_addr #11 {
  %5 = icmp sgt i32 %3, 0
  br i1 %5, label %.preheader26.lr.ph, label %._crit_edge32

.preheader26.lr.ph:                               ; preds = %4
  %6 = zext nneg i32 %3 to i64
  %wide.trip.count42 = zext nneg i32 %3 to i64
  %xtraiter = and i64 %wide.trip.count42, 3
  %7 = icmp ult i32 %3, 4
  %unroll_iter = and i64 %wide.trip.count42, 2147483644
  %lcmp.mod.not = icmp eq i64 %xtraiter, 0
  br label %.preheader.lr.ph

.preheader.lr.ph:                                 ; preds = %._crit_edge30, %.preheader26.lr.ph
  %indvars.iv39 = phi i64 [ 0, %.preheader26.lr.ph ], [ %indvars.iv.next40, %._crit_edge30 ]
  %8 = mul nsw i64 %indvars.iv39, %6
  %9 = and i64 %8, 4294967295
  %invariant.gep = getelementptr float, ptr %0, i64 %9
  %invariant.gep46 = getelementptr float, ptr %2, i64 %8
  br label %.preheader

.preheader:                                       ; preds = %.preheader.lr.ph, %._crit_edge
  %indvars.iv34 = phi i64 [ 0, %.preheader.lr.ph ], [ %indvars.iv.next35, %._crit_edge ]
  %invariant.gep44 = getelementptr float, ptr %1, i64 %indvars.iv34
  br i1 %7, label %._crit_edge.unr-lcssa, label %.preheader.new

.preheader.new:                                   ; preds = %.preheader, %.preheader.new
  %indvars.iv = phi i64 [ %indvars.iv.next.3, %.preheader.new ], [ 0, %.preheader ]
  %.02227 = phi float [ %25, %.preheader.new ], [ 0.000000e+00, %.preheader ]
  %niter = phi i64 [ %niter.next.3, %.preheader.new ], [ 0, %.preheader ]
  %gep = getelementptr float, ptr %invariant.gep, i64 %indvars.iv
  %10 = load float, ptr %gep, align 4
  %11 = mul nsw i64 %indvars.iv, %6
  %gep45 = getelementptr float, ptr %invariant.gep44, i64 %11
  %12 = load float, ptr %gep45, align 4
  %13 = tail call float @llvm.fmuladd.f32(float %10, float %12, float %.02227)
  %indvars.iv.next = or disjoint i64 %indvars.iv, 1
  %gep.1 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next
  %14 = load float, ptr %gep.1, align 4
  %15 = mul nsw i64 %indvars.iv.next, %6
  %gep45.1 = getelementptr float, ptr %invariant.gep44, i64 %15
  %16 = load float, ptr %gep45.1, align 4
  %17 = tail call float @llvm.fmuladd.f32(float %14, float %16, float %13)
  %indvars.iv.next.1 = or disjoint i64 %indvars.iv, 2
  %gep.2 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next.1
  %18 = load float, ptr %gep.2, align 4
  %19 = mul nsw i64 %indvars.iv.next.1, %6
  %gep45.2 = getelementptr float, ptr %invariant.gep44, i64 %19
  %20 = load float, ptr %gep45.2, align 4
  %21 = tail call float @llvm.fmuladd.f32(float %18, float %20, float %17)
  %indvars.iv.next.2 = or disjoint i64 %indvars.iv, 3
  %gep.3 = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.next.2
  %22 = load float, ptr %gep.3, align 4
  %23 = mul nsw i64 %indvars.iv.next.2, %6
  %gep45.3 = getelementptr float, ptr %invariant.gep44, i64 %23
  %24 = load float, ptr %gep45.3, align 4
  %25 = tail call float @llvm.fmuladd.f32(float %22, float %24, float %21)
  %indvars.iv.next.3 = add nuw nsw i64 %indvars.iv, 4
  %niter.next.3 = add i64 %niter, 4
  %niter.ncmp.3 = icmp eq i64 %niter.next.3, %unroll_iter
  br i1 %niter.ncmp.3, label %._crit_edge.unr-lcssa, label %.preheader.new, !llvm.loop !11

._crit_edge.unr-lcssa:                            ; preds = %.preheader.new, %.preheader
  %.lcssa.ph = phi float [ undef, %.preheader ], [ %25, %.preheader.new ]
  %indvars.iv.unr = phi i64 [ 0, %.preheader ], [ %indvars.iv.next.3, %.preheader.new ]
  %.02227.unr = phi float [ 0.000000e+00, %.preheader ], [ %25, %.preheader.new ]
  br i1 %lcmp.mod.not, label %._crit_edge, label %.epil.preheader

.epil.preheader:                                  ; preds = %._crit_edge.unr-lcssa, %.epil.preheader
  %indvars.iv.epil = phi i64 [ %indvars.iv.next.epil, %.epil.preheader ], [ %indvars.iv.unr, %._crit_edge.unr-lcssa ]
  %.02227.epil = phi float [ %29, %.epil.preheader ], [ %.02227.unr, %._crit_edge.unr-lcssa ]
  %epil.iter = phi i64 [ %epil.iter.next, %.epil.preheader ], [ 0, %._crit_edge.unr-lcssa ]
  %gep.epil = getelementptr float, ptr %invariant.gep, i64 %indvars.iv.epil
  %26 = load float, ptr %gep.epil, align 4
  %27 = mul nsw i64 %indvars.iv.epil, %6
  %gep45.epil = getelementptr float, ptr %invariant.gep44, i64 %27
  %28 = load float, ptr %gep45.epil, align 4
  %29 = tail call float @llvm.fmuladd.f32(float %26, float %28, float %.02227.epil)
  %indvars.iv.next.epil = add nuw nsw i64 %indvars.iv.epil, 1
  %epil.iter.next = add i64 %epil.iter, 1
  %epil.iter.cmp.not = icmp eq i64 %epil.iter.next, %xtraiter
  br i1 %epil.iter.cmp.not, label %._crit_edge, label %.epil.preheader, !llvm.loop !16

._crit_edge:                                      ; preds = %.epil.preheader, %._crit_edge.unr-lcssa
  %.lcssa = phi float [ %.lcssa.ph, %._crit_edge.unr-lcssa ], [ %29, %.epil.preheader ]
  %gep47 = getelementptr float, ptr %invariant.gep46, i64 %indvars.iv34
  store float %.lcssa, ptr %gep47, align 4
  %indvars.iv.next35 = add nuw nsw i64 %indvars.iv34, 1
  %exitcond38.not = icmp eq i64 %indvars.iv.next35, %wide.trip.count42
  br i1 %exitcond38.not, label %._crit_edge30, label %.preheader, !llvm.loop !14

._crit_edge30:                                    ; preds = %._crit_edge
  %indvars.iv.next40 = add nuw nsw i64 %indvars.iv39, 1
  %exitcond43.not = icmp eq i64 %indvars.iv.next40, %wide.trip.count42
  br i1 %exitcond43.not, label %._crit_edge32, label %.preheader.lr.ph, !llvm.loop !15

._crit_edge32:                                    ; preds = %._crit_edge30, %4
  ret void
}

; Function Attrs: nofree nounwind
declare noundef i32 @printf(ptr nocapture noundef readonly, ...) local_unnamed_addr #12

; Function Attrs: nounwind
define void @matrix_multiply(ptr nocapture readonly %0, ptr nocapture readonly %1, ptr nocapture writeonly %2, i32 %3) local_unnamed_addr #13 {
entry:
  %4 = load i32, ptr @call_count_v0, align 4
  %5 = load i32, ptr @call_count_v1, align 4
  %6 = load i64, ptr @total_cycles_v0, align 8
  %7 = load i64, ptr @total_cycles_v1, align 8
  %8 = icmp ugt i32 %4, 9
  %9 = icmp ugt i32 %5, 9
  %10 = and i1 %8, %9
  br i1 %10, label %print_best_version.i, label %version_selection.i

print_best_version.i:                             ; preds = %entry
  %11 = zext i32 %5 to i64
  %12 = udiv i64 %7, %11
  %13 = zext i32 %4 to i64
  %14 = udiv i64 %6, %13
  %15 = icmp ult i64 %12, %14
  %16 = select i1 %15, ptr @6, ptr @7
  %17 = tail call i64 @llvm.umin.i64(i64 %12, i64 %14)
  %18 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @0, ptr nonnull %16, i64 %17)
  br i1 %15, label %call_v1.i, label %call_v0.i

call_v0.i:                                        ; preds = %version_selection.i, %print_best_version.i
  %19 = tail call i64 @_ZNSt6chrono3_V212system_clock3nowEv()
  %20 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @1)
  tail call void @matrix_multiply_v0.2(ptr %0, ptr %1, ptr %2, i32 %3)
  %21 = tail call i64 @_ZNSt6chrono3_V212system_clock3nowEv()
  %22 = sub i64 %21, %19
  %23 = add i32 %4, 1
  %24 = add i64 %22, %6
  store i32 %23, ptr @call_count_v0, align 4
  store i64 %24, ptr @total_cycles_v0, align 8
  %25 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @3, i64 %22, i64 %24, i32 %23)
  br label %matrix_multiply_dispatcher.exit

call_v1.i:                                        ; preds = %version_selection.i, %print_best_version.i
  %26 = tail call i64 @_ZNSt6chrono3_V212system_clock3nowEv()
  %27 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @2)
  tail call void @matrix_multiply_v1.4(ptr %0, ptr %1, ptr %2, i32 %3)
  %28 = tail call i64 @_ZNSt6chrono3_V212system_clock3nowEv()
  %29 = sub i64 %28, %26
  %30 = add i32 %5, 1
  %31 = add i64 %29, %7
  store i32 %30, ptr @call_count_v1, align 4
  store i64 %31, ptr @total_cycles_v1, align 8
  %32 = tail call i32 (ptr, ...) @printf(ptr nonnull dereferenceable(1) @3, i64 %29, i64 %31, i32 %30)
  br label %matrix_multiply_dispatcher.exit

version_selection.i:                              ; preds = %entry
  %33 = add i32 %5, %4
  %34 = and i32 %33, 1
  %.not.i = icmp eq i32 %34, 0
  br i1 %.not.i, label %call_v0.i, label %call_v1.i

matrix_multiply_dispatcher.exit:                  ; preds = %call_v0.i, %call_v1.i
  ret void
}

; Function Attrs: nofree nounwind
declare noundef i32 @puts(ptr nocapture noundef readonly) local_unnamed_addr #12

; Function Attrs: nocallback nofree nosync nounwind speculatable willreturn memory(none)
declare i64 @llvm.umin.i64(i64, i64) #14

attributes #0 = { mustprogress nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #1 = { noinline norecurse uwtable "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nobuiltin allocsize(0) "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #3 = { "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #4 = { mustprogress nocallback nofree nounwind willreturn memory(argmem: write) }
attributes #5 = { nounwind "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #6 = { mustprogress noinline uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #7 = { mustprogress noinline nounwind uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #8 = { nofree nounwind "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #9 = { nobuiltin nounwind "frame-pointer"="all" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #10 = { mustprogress nofree noinline norecurse nosync nounwind memory(argmem: readwrite) uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #11 = { mustprogress nofree noinline norecurse nosync nounwind memory(argmem: readwrite) uwtable "frame-pointer"="all" "min-legal-vector-width"="0" "no-trapping-math"="true" "no-unroll-loops"="false" "prefer-vector-width"="256" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #12 = { nofree nounwind }
attributes #13 = { nounwind }
attributes #14 = { nocallback nofree nosync nounwind speculatable willreturn memory(none) }
attributes #15 = { builtin allocsize(0) }
attributes #16 = { builtin nounwind }

!llvm.module.flags = !{!0, !1, !2, !3, !4}
!llvm.ident = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{i32 7, !"frame-pointer", i32 2}
!5 = !{!"Ubuntu clang version 18.1.3 (1ubuntu1)"}
!6 = distinct !{!6, !7, !8, !9}
!7 = !{!"llvm.loop.mustprogress"}
!8 = !{!"llvm.loop.isvectorized", i32 1}
!9 = !{!"llvm.loop.unroll.runtime.disable"}
!10 = distinct !{!10, !7}
!11 = distinct !{!11, !7}
!12 = distinct !{!12, !13}
!13 = !{!"llvm.loop.unroll.disable"}
!14 = distinct !{!14, !7}
!15 = distinct !{!15, !7}
!16 = distinct !{!16, !13}
