// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++, C#, and Java:
// http://www.viva64.com

#include "cbctregistration.h"

#include <qprocess.h> // for calling gPMC from commandline

// configs
#include <itkConfigure.h>

// ITK
#include <itkBinaryThresholdImageFilter.h>
#include <itkGDCMImageIO.h>
#include <itkImageSeriesWriter.h>
#include <itkMinimumMaximumImageFilter.h>
#include <itkMultiplyImageFilter.h>
#include <itkSubtractImageFilter.h>

// PLM
#undef TIMEOUT // used in an enum in dlib, and may be defined as 7 in lp of RTK
#include "xform.h"
#include <dcmtk_rt_study.h>
#include <distance_map.h>
#include <itk_image_load.h>
#include <itk_image_save.h>
#include <itk_mask.h>
#include <itk_threshold.h>
#include <registration.h>
#include <rt_study.h>
#include <rt_study_warp.h>
#include <rtplan_beam.h>
#include <rtplan_control_pt.h>
#include <segment_body.h>
#include <shared_parms.h>
#include <string_util.h>
#include <synthetic_vf.h>
#include <warp_parms.h>

#include "StructureSet.h"
#include "cbctrecon.h"
#include "cbctrecon_io.h"

CbctRegistration::CbctRegistration(CbctRecon *parent) {

  m_pParent = parent;

  m_pDcmStudyPlan = nullptr;
  WEPL_voi = std::make_unique<Rtss_roi_modern>();
  cur_voi = std::make_unique<Rtss_roi_modern>();
}

CbctRegistration::~CbctRegistration() {

  for (auto i = 0; i < 3; i++) {
    m_YKImgFixed[i].ReleaseBuffer();
    m_YKImgMoving[i].ReleaseBuffer();
    m_YKDisp[i].ReleaseBuffer();
    m_DoseImgFixed[i].ReleaseBuffer();
    m_DoseImgMoving[i].ReleaseBuffer();
    m_AGDisp_Overlay[i].ReleaseBuffer();
  }

  if (m_pDcmStudyPlan != nullptr) {
    delete m_pDcmStudyPlan;
    m_pDcmStudyPlan = nullptr;
  }
}

void CbctRegistration::GenPlastiRegisterCommandFile(
    const QString &strPathCommandFile, const QString &strPathFixedImg,
    const QString &strPathMovingImg, const QString &strPathOutImg,
    const QString &strPathXformOut, const enRegisterOption regiOption,
    const QString &strStageOption1, const QString &strStageOption2,
    const QString &strStageOption3, const QString &strPathFixedMask,
    const bool optim_mse, const bool use_cuda,
    QString & /*GradOptionStr*/) const {
  // optionStr = ui.lineEditGradOption->text();
  std::ofstream fout;
  fout.open(strPathCommandFile.toLocal8Bit().constData());

  if (fout.fail()) {
    std::cout << "File writing error! " << std::endl;
    return;
  }

  fout << "# command_file.txt" << std::endl;
  fout << "[GLOBAL]" << std::endl;
  fout << "fixed=" << strPathFixedImg.toLocal8Bit().constData() << std::endl;
  fout << "moving=" << strPathMovingImg.toLocal8Bit().constData() << std::endl;

  if (strPathFixedMask.length() > 1) {
    fout << "fixed_roi=" << strPathFixedMask.toLocal8Bit().constData()
         << std::endl;
  }

  fout << "img_out=" << strPathOutImg.toLocal8Bit().constData() << std::endl;
  fout << "xform_out=" << strPathXformOut.toLocal8Bit().constData()
       << std::endl;
  if (regiOption == PLAST_GRADIENT) {
    fout << "logfile="
         << "gradient_log.txt" << std::endl;
  } else if (regiOption == PLAST_RIGID) {
    fout << "logfile="
         << "rigid_log.txt" << std::endl;
  } else if (regiOption == PLAST_BSPLINE) {
    fout << "logfile="
         << "bspline_log.txt" << std::endl;
  }

  fout << std::endl;

  // QString strOptim = ui.comboBoxDeformOption->currentText();
  QString strOptim;
  if (optim_mse) { // ui.radioButton_mse->isChecked()) {
    strOptim = "mse";
  } else { // if (ui.radioButton_mi->isChecked()) {
    strOptim = "mi";
  }

  auto strListOption1 =
      strStageOption1.split(","); // Subsampling rate (3), Grid
                                  // size (1), Regularization(1),
                                  // LandmarkPenalty(1), Max
                                  // Iteration (1), StageOutput(1)
  auto strListOption2 = strStageOption2.split(",");
  auto strListOption3 = strStageOption3.split(",");

  QString optionStr;
  QStringList optionList;

  std::string treading_opt = "openmp";
  if (use_cuda) { // m_pParent->ui.radioButton_UseCUDA->isChecked()) {
    treading_opt = "cuda";
  }

  switch (regiOption) {
  case PLAST_RIGID:
    //         "max_step=" << "0.05\n";
    //         "optim=" << "amoeba\n";
    //         "optim=" << "rsg\n";
    //         "optim=" << "amoeba\n";
    //         "impl=" << "plastimatch\n";
    //         "metric=" << "mi\n"; //added
    //         "impl=" << "itk\n";

    //        "impl="
    //     << "itk\n";
    //         "background_val=" << "0\n";
    fout << "[STAGE]\n"
            "xform=rigid\n"
            "optim=versor\n";
    fout << "threading=" << treading_opt << "\n";
    fout << "background_val=500\n" //-600 in HU //added
            "max_its=70\n";

    break;
  case PLAST_GRADIENT:
    fout << "#For gradient-based searching, moving image should be smaller "
            "than fixed image. So, CBCT image might move rather than CT\n";

    optionList = optionStr.split(",");

    fout << "[PROCESS]\n"
            "action=adjust\n"
            "# only consider within this  intensity values\n"
            "parms=-inf,0,-1000,-1000,4000,4000,inf,0\n"
            "images=fixed,moving\n\n"
            "[STAGE]\n"
            "metric=gm\n"
            "xform=translation\n"
            "optim=grid_search\n";
    fout << "gridsearch_min_overlap=" << optionList.at(0).toDouble() << " "
         << optionList.at(1).toDouble() << " " << optionList.at(2).toDouble()
         << "\n";

    fout << "num_substages=5\n";
    fout << "debug_dir=" << m_strPathPlastimatch.toLocal8Bit().constData()
         << "\n";
    break;

  case PLAST_BSPLINE:
    if (strListOption1.count() == 8) {
      fout << "[STAGE]\n"
              "xform=bspline\n"
              "impl=plastimatch\n";

      fout << "threading=" << treading_opt << "\n";
      if (optim_mse) {
        fout << "alg_flavor=j\n";
      } else {
        fout << "alg_flavor=a\n";
      }

      fout << "regularization_lambda=" << strListOption1.at(4).toDouble()
           << "\n";

      if (strOptim.length() < 1) {
        fout << "metric=mi\n";
      } else {
        fout << "metric=" << strOptim.toLocal8Bit().constData() << "\n";
      }

      fout << "max_its=" << strListOption1.at(6).toInt() << "\n";
      const auto grid_spacing = strListOption1.at(3).toInt();
      fout << "grid_spac=" << grid_spacing << " " << grid_spacing << " "
           << grid_spacing << "\n"; // 20 20 20 --> minimum
      fout << "res=" << strListOption1.at(0).toInt() << " "
           << strListOption1.at(1).toInt() << " "
           << strListOption1.at(2).toInt() << "\n";
      fout << "background_val=700\n"; //-600 in HU //added

      fout << "img_out=" << strListOption1.at(7).toLocal8Bit().constData()
           << "\n\n";
    }
    if (strListOption2.count() == 8) {
      fout << "[STAGE]\n"
              "xform=bspline\n"
              "impl=plastimatch\n";

      fout << "threading=" << treading_opt << "\n";

      fout << "regularization_lambda=" << strListOption2.at(4).toDouble()
           << "\n";

      if (strOptim.length() < 1) {
        fout << "metric=mi\n";
      } else {
        fout << "metric=" << strOptim.toLocal8Bit().constData() << "\n";
      }

      fout << "max_its=" << strListOption2.at(6).toInt() << "\n";
      const auto grid_spacing = strListOption2.at(3).toInt();
      fout << "grid_spac=" << grid_spacing << " " << grid_spacing << " "
           << grid_spacing << "\n"; // 20 20 20 --> minimum
      fout << "res=" << strListOption2.at(0).toInt() << " "
           << strListOption2.at(1).toInt() << " "
           << strListOption2.at(2).toInt() << "\n";

      fout << "img_out=" << strListOption2.at(7).toLocal8Bit().constData()
           << "\n\n";
    }
    if (strListOption3.count() == 8) {
      fout << "[STAGE]\n"
              "xform=bspline\n"
              "impl=plastimatch\n";

      fout << "threading=" << treading_opt << "\n";

      fout << "regularization_lambda=" << strListOption3.at(4).toDouble()
           << "\n";

      if (strOptim.length() < 1) {
        fout << "metric=mi\n";
      } else {
        fout << "metric=" << strOptim.toLocal8Bit().constData() << "\n";
      }

      fout << "max_its=" << strListOption3.at(6).toInt() << "\n";
      const auto grid_spacing = strListOption3.at(3).toInt();
      fout << "grid_spac=" << grid_spacing << " " << grid_spacing << " "
           << grid_spacing << "\n"; // 20 20 20 --> minimum
      fout << "res=" << strListOption3.at(0).toInt() << " "
           << strListOption3.at(1).toInt() << " "
           << strListOption3.at(2).toInt() << "\n";

      fout << "img_out=" << strListOption3.at(7).toLocal8Bit().constData()
           << "\n";
      fout << std::endl;
    }

    break;

  case PLAST_AFFINE:
    std::cerr << "PLAST_AFFINE not implemented, please use gradient, rigid or "
                 "bspline instead."
              << std::endl;
    break;
  }

  fout.close();
  // Sleep(1000); //Just in case.. it seems it helped to avoid random crash!
}

void CbctRegistration::autoPreprocessCT(const int iAirThresholdShort,
                                        UShortImageType::Pointer &spFixed,
                                        UShortImageType::Pointer &spMoving) {

  const auto air_thresh =
      static_cast<unsigned short>(1024 + iAirThresholdShort);
  std::cout << "Thresh: " << air_thresh << std::endl;
  using threshFilterType =
      itk::BinaryThresholdImageFilter<UShortImageType, ShortImageType>;
  auto threshFilter_CT = threshFilterType::New();
  threshFilter_CT->SetInput(spMoving);

  threshFilter_CT->SetOutsideValue(0);
  threshFilter_CT->SetInsideValue(1);
  threshFilter_CT->SetLowerThreshold(air_thresh); // -600 HU
  // threshFilter_CT->Update();

  auto threshFilter_CBCT = threshFilterType::New();
  threshFilter_CBCT->SetInput(spFixed);

  threshFilter_CBCT->SetOutsideValue(0);
  threshFilter_CBCT->SetInsideValue(1);
  threshFilter_CBCT->SetLowerThreshold(air_thresh); // -600 HU
  // threshFilter_CBCT->Update();

  using subFilterType =
      itk::SubtractImageFilter<ShortImageType, ShortImageType, ShortImageType>;
  auto subFilter = subFilterType::New();
  subFilter->SetInput1(threshFilter_CT->GetOutput());
  subFilter->SetInput2(threshFilter_CBCT->GetOutput());
  std::cout << "Making fill-crop mask..." << std::endl;
  try {
    subFilter->Update();
  } catch (std::exception &e) {
    std::cerr << "Failed to sub threshold masks: " << e.what() << std::endl;
    return;
  }
  // -1 -> should be filled
  // +1 -> should be cropped
  std::cout << "done." << std::endl;

  using iteratorType = itk::ImageRegionConstIterator<ShortImageType>;
  iteratorType it(subFilter->GetOutput(),
                  subFilter->GetOutput()->GetLargestPossibleRegion());

  using CTiteratorType = itk::ImageRegionIterator<UShortImageType>;
  CTiteratorType CT_it(spMoving, spMoving->GetLargestPossibleRegion());

  std::cout << "Overwriting values..." << std::endl;
  it.GoToBegin();
  while (!it.IsAtEnd()) {
    const int val = it.Get();
    if (val == -1) {
      CT_it.Set(1024U); // water
    } else if (val == 1) {
      CT_it.Set(0U); // air
    }
    ++it;
    ++CT_it;
  }
  std::cout << "done." << std::endl;
}

bool CbctRegistration::PreprocessCT(
    const int iAirThresholdShort, QString strRSName, const bool fill_bubble,
    const int iBubbleFillingVal,
    const int iAirFillValShort) // CT preparation + CBCT preparation only,
                                // then show the registration DLG
{
  // All files will be saved in m_strPathPlastimatch

  // 1) CT preparation
  // string strCTDir = m_pParent->m_strPathPlanCTDir.toStdString();
  // string strPathMaskBubbleCT =
  // m_strPathPlastimatch.append("/msk_bubbles_CT.mha").toStdString();
  auto strPathMskBubbleCT = m_strPathPlastimatch + "/msk_bubbles_CT.mha";

  // char* strTest = (char*)(strCTDir.c_str());
  // const char* strTest2 = (strCTDir.c_str());
  // int iAirThresholdShort = -600;
  //  Segment_parms parms;

  /* [1]Segment air region*/
  Plm_image in;
  Plm_image out;
  // Segment_body *sb = &parms->sb;
  Segment_body sb;
  sb.m_lower_threshold = static_cast<float>(iAirThresholdShort);

  /* Load the input image */
  const auto &ct_path = m_pParent->m_strPathPlanCTDir.toLocal8Bit().constData();
  in.load_native(ct_path);

  sb.img_in = &in;
  sb.img_out = &out;
  /* Do segmentation */
  sb.do_segmentation_air_cavity();
  /* Save output file */
  sb.img_out->save_image(strPathMskBubbleCT.toLocal8Bit().constData());

  if (m_pParent->m_strPathRS.isEmpty()) {
    return false;
  }
  /* End of [1]Segment air region*/

  // plastimatch convert --input E:\PlastimatchData\DicomEg\OLD\RS.dcm
  // --output-ss-img E:\PlastimatchData\DicomEg\OLD\ssimg_all2.mha
  // --output-ss-list E:\PlastimatchData\DicomEg\OLD\sslist_all.txt
  // --referenced-ct E:\PlastimatchData\DicomEg\OLD\CT

  // plm_clp_parse (&parms, &parse_fn, &usage_fn, argc, argv, 1)
  // plastimatch segment --input E:\PlastimatchData\DicomEg\OLD\CT --output-img
  // E:\PlastimatchData\DicomEg\OLD\msk_bubbles_oldCT.mha --lower-threshold -600

  // do_command_warp(argc, argv);

  /* [2]Load RS file to make a Skin mask*/
  Warp_parms parms;
  Rt_study rtds;

  parms.input_fn = m_pParent->m_strPathRS.toLocal8Bit().constData();
  parms.referenced_dicom_dir = ct_path;
  std::cout << ct_path << std::endl;

  auto ssimg_path_all = m_strPathPlastimatch + "/ssimg_all.mha";
  auto sslist_path_all = m_strPathPlastimatch + "/sslist_all.txt";
  parms.output_ss_img_fn = ssimg_path_all.toLocal8Bit().constData();
  parms.output_ss_list_fn = sslist_path_all.toLocal8Bit().constData();

  parms.prefix_format = "mha";
  parms.use_itk = 0;
  parms.interp_lin = 1;

  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type = PLM_FILE_FMT_DICOM_RTSS;
  /*if (file_type == PLM_FILE_FMT_POINTSET) {
  warp_pointset_main (&parms);
  return;
  }*/
  /* Process warp */
  rt_study_warp(&rtds, file_type, &parms);
  printf("Warping Finished!\n");

  // return;

  /* [3]Read outputss-list.txt and leave skin only*/
  std::ifstream fin;
  fin.open(sslist_path_all.toLocal8Bit().constData(), std::ios::in);
  if (fin.fail()) {
    return false;
  }

  char str[MAX_LINE_LENGTH];

  QString strLineSkin;

  strRSName = strRSName.trimmed();

  while (!fin.eof()) {
    memset(str, 0, MAX_LINE_LENGTH);
    fin.getline(str, MAX_LINE_LENGTH);
    QString strLine(str);

    auto strList = strLine.split('|');
    // third one is the organ name
    if (strList.length() != 3) {
      std::cout << "abnormal file expression." << std::endl;
      break;
    }
    auto organName = strList.at(2);

    organName = organName.trimmed();
    // YKTEMP20150922
    // if (organName == "Skin" || organName == "skin" || organName == "SKIN")
    //{
    //  strLineSkin = strLine;
    //}

    // if (organName == strRSName)
    if (organName.compare(strRSName, Qt::CaseInsensitive) == 0) {
      strLineSkin = strLine;
      std::cout << "Structure for cropping was found: "
                << strLineSkin.toLocal8Bit().constData() << std::endl;
    }
  }
  fin.close();

  QString sslist_path_skin;
  if (strLineSkin.length() > 1) {
    std::ofstream fout;
    sslist_path_skin = sslist_path_all;
    sslist_path_skin.replace("all", "skin");
    fout.open(sslist_path_skin.toLocal8Bit().constData());

    fout << strLineSkin.toLocal8Bit().constData() << std::endl;

    fout.close();
  } else {
    std::cout << "Error: no " << strRSName.toLocal8Bit().constData()
              << " RS structure is found in DICOM RS file. " << std::endl;
    return false;
  }
  /* End of [3]Read outputss-list.txt and leave skin only*/

  /* [4]prepare a skin mask image*/
  // plastimatch convert --input-ss-img [ssimg_all.mha] --input-ss-list
  // [sslist_skin.txt] --output-labelmap [msk_skin.mha]

  Warp_parms parms2;
  Rt_study rtds2;
  // convert --input-ss-img E:\PlastimatchData\DicomEg\OLD\ssimg_all.mha
  // --input-ss-list E:\PlastimatchData\DicomEg\OLD\sslist_skin.txt
  // --output-labelmap E:\PlastimatchData\DicomEg\OLD\msk_skin.mha
  auto strPath_mskSkinCT = m_strPathPlastimatch + "/msk_skin_CT.mha";
  parms2.input_ss_img_fn = ssimg_path_all.toLocal8Bit().constData();
  parms2.input_ss_list_fn = sslist_path_skin.toLocal8Bit().constData();
  parms2.output_labelmap_fn =
      strPath_mskSkinCT.toLocal8Bit().constData(); // output
  parms2.prefix_format = "mha";
  parms2.use_itk = 0;
  parms2.interp_lin = 1;
  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type2 = PLM_FILE_FMT_NO_FILE;
  /* Process warp */
  rt_study_warp(&rtds2, file_type2, &parms2);
  printf("Warping2 Finished!\n");

  m_strPathCTSkin = strPath_mskSkinCT;

  /* [5] prepare a contracted skin (5 mm)*/

  // Bubble removal procedure
  const auto str_path_bubble_removed_ct =
      m_strPathPlastimatch + "/bubble_filled_CT.mha";
  auto str_path_msk_skin_ct_cont =
      m_strPathPlastimatch + "/msk_skin_CT_cont.mha";
  const auto str_path_msk_bubble_ct_fin =
      m_strPathPlastimatch + "/msk_bubbles_CT_fin.mha";

  Mask_operation mask_option;
  QString input_fn;
  QString mask_fn;
  QString output_fn;
  auto mask_value = 0.0f;

  if (fill_bubble) {
    // QString strPath_mskSkinCT_dmap =  m_strPathPlastimatch +
    // "/dmap_msk_skin_CT.mha";
    plm_expansion_contract_msk(strPath_mskSkinCT, str_path_msk_skin_ct_cont,
                               -5.0); //-5mm contraction

    // Mask_parms parms_msk;
    /* Check if we're doing fill or mask */
    mask_option = MASK_OPERATION_MASK;
    /* Input files */
    input_fn = strPathMskBubbleCT;
    mask_fn = str_path_msk_skin_ct_cont;
    output_fn = str_path_msk_bubble_ct_fin;

    // plm_mask_main(&parms_msk);
    plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

    // Mask_parms parms_msk2;

    mask_option = MASK_OPERATION_FILL;
    input_fn = m_pParent->m_strPathPlanCTDir;
    mask_fn = str_path_msk_bubble_ct_fin;
    output_fn = str_path_bubble_removed_ct;
    mask_value = static_cast<float>(iBubbleFillingVal);

    plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);
  }

  /* - remove outside air*/
  // plastimatch mask --input
  // E:\PlastimatchData\DicomEg\OLD\CT_bubble_removed.mha  --mask-value -1024
  // --mask E:\PlastimatchData\DicomEg\OLD\msk_skin.mha --output
  // E:\PlastimatchData\DicomEg\OLD\CT_final.mha

  // Mask_parms parms_msk3;
  auto strPathSkinRemovedCT = m_strPathPlastimatch + "/skin_removed_CT.mha";

  // parms_msk3.mask_operation = MASK_OPERATION_MASK;

  mask_option = MASK_OPERATION_MASK;

  QFileInfo tmpInfo(str_path_bubble_removed_ct);
  if (tmpInfo.exists()) {
    input_fn = str_path_bubble_removed_ct;
  } else {
    input_fn = m_pParent->m_strPathPlanCTDir;
  }

  mask_fn = strPath_mskSkinCT;
  output_fn = strPathSkinRemovedCT;
  mask_value = static_cast<float>(iAirFillValShort);
  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

  // strPathSkinRemovedCT .mha file is ready. this is SHORT image

  LoadShortImageToUshort(strPathSkinRemovedCT, m_pParent->m_spRefCTImg);
  m_pParent->RegisterImgDuplication(REGISTER_REF_CT, REGISTER_MANUAL_RIGID);
  // m_pParent->SLT_ViewRegistration();

  // Update the recon resolution (for X and Y)?--> doesn't matter, since the
  // regidbody registration will match the resolution

  // if (m_pDcmStudyPlan != NULL)
  //{
  //    Rtplan::Pointer rtplan = m_pDcmStudyPlan->get_rtplan();
  //    int iCntBeam = rtplan->num_beams;
  //    //Get Isocenter position for first beam

  //}

  return true;
}

void CbctRegistration::LoadRTPlan(QString &strDCMPath) {
  if (strDCMPath.length() < 1) {
    std::cout << "No dicom  file is found" << std::endl;
    return;
  }

  if (m_pDcmStudyPlan != nullptr) {
    delete m_pDcmStudyPlan;
    m_pDcmStudyPlan = nullptr;
  }

  m_pDcmStudyPlan = new Dcmtk_rt_study();

  // std::cout << "Before plm_file_format_deduce" << std::endl;
  const auto file_type_dcm_plan =
      plm_file_format_deduce(strDCMPath.toLocal8Bit().constData());
  // std::cout << "After plm_file_format_deduce" << std::endl;

  if (file_type_dcm_plan == PLM_FILE_FMT_DICOM_RTPLAN) {
    std::cout << "PLM_FILE_FMT_DICOM_RTPLAN "
              << "is found" << std::endl;
    m_pDcmStudyPlan->load(strDCMPath.toLocal8Bit().constData());
  } else {
    std::cout << "Found file is not RTPLAN. Skipping dcm plan." << std::endl;
  }

  // Rtplan::Pointer rtplan = m_pDcmStudyPlan->get_rtplan();
}

void CbctRegistration::CalculateWEPLtoVOI(const std::string &voi_name,
                                          const int gantry_angle,
                                          const int couch_angle,
                                          UShortImageType::Pointer &spMoving,
                                          UShortImageType::Pointer &spFixed) {
  if (voi_name.length() < 1) {
    std::cout << "No VOI name given" << std::endl;
    return;
  }
  cur_voi = m_pParent->m_structures->get_ss(PLAN_CT)->get_roi_by_name(voi_name);

  // Get basis from angles
  const auto vec_basis = get_basis_from_angles(gantry_angle, couch_angle);

  // Get Fixed and Moving
  // Tranlate fixed and moving to dEdx
  const auto wepl_cube = ConvertUshort2WeplFloat(spMoving);
  const auto wepl_cube_fixed = ConvertUshort2WeplFloat(spFixed);

  // Initialize WEPL contour
  WEPL_voi = std::make_unique<Rtss_roi_modern>();
  WEPL_voi->name = "WEPL" + voi_name;
  WEPL_voi->color = "255 0 0";
  WEPL_voi->id = cur_voi->id;   /* Used for import/export (must be >= 1) */
  WEPL_voi->bit = cur_voi->bit; /* Used for ss-img (-1 for no bit) */
  WEPL_voi->num_contours = cur_voi->num_contours;
  WEPL_voi->pslist.resize(WEPL_voi->num_contours);

  auto i = 0U;
  // Calculate WEPL
  for (const auto &contour : cur_voi->pslist) {
    auto WEPL_contour = std::make_unique<Rtss_contour_modern>(contour);
    WEPL_contour->ct_slice_uid = contour.ct_slice_uid;
    WEPL_contour->slice_no = contour.slice_no;
    WEPL_contour->num_vertices = contour.num_vertices;
    // Actually calculate WEPL on spMoving
    auto WEPL_points =
        WEPLContourFromRtssContour(contour, vec_basis, wepl_cube);

    // Inversely calc WEPL on spFixed
    // And put WEPL point in contour
    std::transform(std::begin(WEPL_points), std::end(WEPL_points),
                   std::begin(WEPL_contour->coordinates),
                   [&vec_basis, &wepl_cube_fixed](const WEPLVector &val) {
                     return NewPoint_from_WEPLVector(val, vec_basis,
                                                     wepl_cube_fixed);
                   });
    WEPL_voi->pslist.at(i++) = std::move(WEPL_contour);
  }
}

float *CbctRegistration::ManualMoveByDCM() const {

  if (m_pParent->m_spRefCTImg == nullptr ||
      m_pParent->m_spManualRigidCT == nullptr) {
    return nullptr;
  }

  if (m_pDcmStudyPlan == nullptr) {
    std::cout << "Error! no dcmRTStudy is loaded" << std::endl;
    return nullptr;
  }

  auto rtplan = m_pDcmStudyPlan->get_rtplan();

  if (!rtplan) {
    std::cout << "Error! no dcm plan is loaded" << std::endl;
    return nullptr;
  }
  const auto iCntBeam = rtplan->beamlist.size(); //->num_beams;

  if (iCntBeam < 1) {
    std::cout << "Error! no beam is found" << std::endl;
    return nullptr;
  }

  float *final_iso_pos = nullptr;

  for (size_t i = 0; i < iCntBeam; i++) {
    auto curBeam = rtplan->beamlist[i];

    const auto iCntCP = curBeam->cplist.size(); // num_cp;

    for (size_t j = 0; j < iCntCP; j++) {
      const auto cur_iso_pos = curBeam->cplist[j]->get_isocenter();
      //                ID                id                               ID
      std::cout << "Beam Gantry: " << curBeam->gantry_angle
                << ", Control point rate: " << curBeam->cplist[j]->meterset_rate
                << // control_pt_no <<
          ", Isocenter pos : " << cur_iso_pos[0] << "/" << cur_iso_pos[1] << "/"
                << cur_iso_pos[2] << std::endl;

      if (i == 0 && j == 0) {
        final_iso_pos = curBeam->cplist[j]->get_isocenter();
      }
    }
  }
  // VEC3D shiftVal;// = GetShiftValueFromGradientXForm(filePathXform, true);
  // //true: inverse trans should be applied if CBCT was moving image //in mm
  return final_iso_pos;
}

// void CbctRegistration::plm_dmap_main( Dmap_parms* parms )
void CbctRegistration::plm_dmap_main(QString &img_in_fn,
                                     QString &img_out_fn) const {
  if (img_in_fn.length() < 1 || img_out_fn.length() < 1) {
    return;
  }
  // dmap_main (&parms_dmap);
  Distance_map dmap;
  dmap.set_input_image(img_in_fn.toLocal8Bit().constData());
  // dmap.set_algorithm (parms_dmap.algorithm);
  // dmap.set_inside_is_positive (parms->inside_positive);
  // dmap.set_use_squared_distance (parms->squared_distance);
  dmap.run();
  const auto dmap_image = dmap.get_output_image();
  itk_image_save(dmap_image, img_out_fn.toLocal8Bit().constData());
}

// void CbctRegistration::plm_threshold_main(Pcmd_threshold* parms)
void CbctRegistration::plm_threshold_main(QString &strRange, QString &img_in_fn,
                                          QString &img_out_fn) const {
  /*if (parms == NULL)
        return;*/

  if (img_in_fn.length() < 1 || img_out_fn.length() < 1) {
    return;
  }

  // threshold_main (&parms_thre);
  const auto plm_image = plm_image_load(img_in_fn.toLocal8Bit().constData(),
                                        PLM_IMG_TYPE_ITK_FLOAT);
  const auto img_in = plm_image->m_itk_float;
  UCharImageType::Pointer img_out;

  if (!strRange.isEmpty()) {
    img_out =
        itk_threshold(img_in, std::string(strRange.toLocal8Bit().constData()));
  }

  Plm_image pli(img_out);
  pli.save_image(img_out_fn.toLocal8Bit().constData());

  // bool output_dicom = false;
  // enum output_type = PLM_IMG_TYPE_UNDEFINED; //

  // if (output_dicom) {
  // itk_image_save_short_dicom (
  //  img_out, img_out_fn.toLocal8Bit().constData(), 0);
  // } else {
  // Plm_image pli (img_out);

  // if (output_type) {
  //           pli.convert(output_type);
  //}
  // pli.save_image (img_out_fn);
  // }
  std::cout << "contracted skin mask is ready" << std::endl;
}

// void CbctRegistration::plm_mask_main(Mask_parms* parms)
void CbctRegistration::plm_mask_main(const Mask_operation mask_option,
                                     QString &input_fn, QString &mask_fn,
                                     QString &output_fn,
                                     const float mask_value) const {
  auto img = plm_image_load_native(input_fn.toLocal8Bit().constData());
  if (!img) {
    printf("Error: could not open '%s' for read\n",
           input_fn.toLocal8Bit().constData());
    return;
  }

  const auto mask =
      itk_image_load_uchar(mask_fn.toLocal8Bit().constData(), nullptr);

  switch (img->m_type) {
  case PLM_IMG_TYPE_ITK_UCHAR:
    img->m_itk_uchar =
        mask_image(img->m_itk_uchar, mask, mask_option, mask_value);
    break;
  case PLM_IMG_TYPE_ITK_SHORT:
    img->m_itk_short =
        mask_image(img->m_itk_short, mask, mask_option, mask_value);
    break;
  case PLM_IMG_TYPE_ITK_USHORT:
    img->m_itk_ushort =
        mask_image(img->m_itk_ushort, mask, mask_option, mask_value);
    break;
  case PLM_IMG_TYPE_ITK_ULONG:
    img->m_itk_uint32 =
        mask_image(img->m_itk_uint32, mask, mask_option, mask_value);
    break;
  case PLM_IMG_TYPE_GPUIT_FLOAT:
  case PLM_IMG_TYPE_ITK_FLOAT:
    img->m_itk_float =
        mask_image(img->itk_float(), mask, mask_option, mask_value);
    break;
  default:
    printf("Unhandled conversion in mask_main\n");
    break;
  }

  const auto output_dicom = false; // default: comes from Mask_param header
  // const auto output_type =
  //    PLM_IMG_TYPE_UNDEFINED; // default: comes from Mask_param header

  if (!output_dicom) {
    // if (output_type != 0) {
    //  img->convert(output_type);
    //}
    img->save_image(output_fn.toLocal8Bit().constData());
    //} else {
    //  img->save_short_dicom(output_fn.toLocal8Bit().constData(), nullptr);
  }
}

void CbctRegistration::plm_expansion_contract_msk(QString &strPath_msk,
                                                  QString &strPath_msk_exp_cont,
                                                  const double fExpVal) const {
#if defined(commentout)
  Dmap_parms parms_dmap; // orignally, Dmap_parms is defined in pcmd_dmap.cxx,
                         // supposed to be in any .h
  QString strPath_mskSkinCT_dmap = strPath_msk + "_dmap.mha";
  parms_dmap.img_in_fn = strPath_msk.toLocal8Bit().constData();
  parms_dmap.img_out_fn = strPath_mskSkinCT_dmap.toLocal8Bit().constData();
  plm_dmap_main(&parms_dmap);
#endif

  auto strPath_mskSkinCT_dmap = strPath_msk + "_dmap.mha";
  plm_dmap_main(strPath_msk, strPath_mskSkinCT_dmap);

  // Thresholding
  /* Pcmd_threshold parms_thre;

   parms_thre.range_string = string_format ("-inf,%f", fExpVal);
   parms_thre.img_in_fn = strPath_mskSkinCT_dmap.toLocal8Bit().constData();
   parms_thre.img_out_fn = strPath_mskSkinCT_mod.toLocal8Bit().constData();   */

  const auto &strPath_mskSkinCT_mod = strPath_msk_exp_cont;
  auto range_string = QString(string_format("-inf,%f", fExpVal).c_str());
  auto img_in_fn = strPath_mskSkinCT_dmap;
  auto img_out_fn = strPath_mskSkinCT_mod;

  // plm_threshold_main(&parms_thre);
  plm_threshold_main(range_string, img_in_fn, img_out_fn);
}

// NoBubble filling is included. bubble is only needed to be filled during
// deformable regi
void CbctRegistration::ProcessCBCT_beforeAutoRigidRegi(
    QString &strPathRawCBCT, QString &strPath_mskSkinCT,
    QString &strPathOutputCBCT, double *manualTrans3d,
    const bool bPrepareMaskOnly, double skinExp, const int bkGroundValUshort) {
  // Calc. Origin difference

  // 1) Move CT mask according to the manual shift
  // plastimatch synth-vf --fixed [msk_skin.mha] --output [xf_manual_trans.mha]
  // --xf-trans "[origin diff (raw - regi)]"
  if (m_pParent->m_spManualRigidCT == nullptr) {
    return;
  }

  // QString strPath_mskSkinCT = m_strPathPlastimatch + "/msk_skin_CT.mha";
  auto strPath_outputXF_manualTrans =
      m_strPathPlastimatch + "/xf_manual_trans.mha";

  // USHORT_ImageType::PointType rawOrigin =
  // m_pParent->m_spReconImg->GetOrigin();  USHORT_ImageType::PointType
  // rigidMovedOrigin = m_pParent->m_spManualRigidCT->GetOrigin();
  //
  // double trans[3];
  // trans[0] = (double)(rawOrigin[0] - rigidMovedOrigin[0]);
  // trans[1] = (double)(rawOrigin[1] - rigidMovedOrigin[1]);
  // trans[2] = (double)(rawOrigin[2] - rigidMovedOrigin[2]);

  plm_synth_trans_xf(strPath_mskSkinCT, strPath_outputXF_manualTrans,
                     manualTrans3d[0], manualTrans3d[1], manualTrans3d[2]);

  // 2) Move CT mask according to the manual shift
  /*Move the skin contour according to the std::vector field
        plastimatch warp --input [msk_skin.mha] --output-img
  [msk_skin_manRegi.mha] --xf [xf_manual_trans.mha] plastimatch warp --input
  E:\PlastimatchData\DicomEg\OLD\msk_skin.mha --output-img
  E:\PlastimatchData\DicomEg\OLD\msk_skin_manRegi.mha --xf
  E:\PlastimatchData\DicomEg\OLD\xf_manual_trans.mha*/

  // parms->output_img_fn = parser->get_string("output-img").c_str();
  // parms->xf_in_fn = parser->get_string("xf").c_str();

  // convert --input-ss-img E:\PlastimatchData\DicomEg\OLD\ssimg_all.mha
  // --input-ss-list E:\PlastimatchData\DicomEg\OLD\sslist_skin.txt
  // --output-labelmap E:\PlastimatchData\DicomEg\OLD\msk_skin.mha  QString
  // strPath_mskSkinCT = m_strPathPlastimatch + "/msk_skin_CT.mha";
  auto strPath_mskSkinCT_manRegi =
      m_strPathPlastimatch + "/msk_skin_CT_manRegi.mha";

  Warp_parms parms;
  Rt_study rtds;

  parms.input_fn = strPath_mskSkinCT.toLocal8Bit().constData();
  parms.output_img_fn = strPath_mskSkinCT_manRegi.toLocal8Bit().constData();
  parms.xf_in_fn = strPath_outputXF_manualTrans.toLocal8Bit().constData();
  parms.fixed_img_fn = strPathRawCBCT.toLocal8Bit()
                           .constData(); // add this to fit Mask to CBCT image

  parms.prefix_format = "mha";
  parms.use_itk = 0;
  parms.interp_lin = 1; // linear
  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type = PLM_FILE_FMT_IMG;

  std::cout << "Entering plm rt_study_warp..." << std::endl;

  rt_study_warp(&rtds, file_type, &parms);
  printf("Warping_rigid_trans Finished!\n");

  // 3) Expand 10mm skin contour
  // plastimatch synth-vf --fixed
  // E:\PlastimatchData\DicomEg\OLD\msk_skin_manRegi.mha --radial-mag "0 0 0"
  // --xf-radial --output E:\PlastimatchData\DicomEg\NEW\xf_exp_CB.mha
  // plastimatch synth-vf --fixed
  // E:\PlastimatchData\DicomEg\OLD\msk_skin_manRegi.mha --radial-mag "0.1 0.1
  // 0.1" --xf-radial --output E:\PlastimatchData\DicomEg\NEW\xf_exp_CB.mha

  auto strPath_mskSkinCT_manRegi_exp =
      m_strPathPlastimatch + "/msk_skin_CT_manRegi_exp.mha";

  if (skinExp < 0.0 || skinExp > 100.0) {
    skinExp = 0.0;
  }

  plm_expansion_contract_msk(strPath_mskSkinCT_manRegi,
                             strPath_mskSkinCT_manRegi_exp,
                             skinExp); // 10 mm expansion for a mask image

  // 4) eliminate the air region (temporarily)
  // plastimatch mask --input E:\PlastimatchData\DicomEg\NEW\rawCBCT2.mha
  // --mask-value 0 --mask
  // E:\PlastimatchData\DicomEg\OLD\msk_skin_autoRegi_exp.mha --output
  // E:\PlastimatchData\DicomEg\NEW\rawCBCT4.mha

  // std::cout << "bPrepareMaskOnly=" << bPrepareMaskOnly << std::endl;

  // Mask_parms parms_msk;
  // QString strPath_CBCT_skinRemovedTemp = m_strPathPlastimatch +
  // "/skin_removed_CBCT_tmp.mha";

  const auto mask_option = MASK_OPERATION_MASK;
  auto mask_fn = strPath_mskSkinCT_manRegi_exp;
  // parms_msk.mask_value = 0.0; //unsigned short
  const float mask_value = bkGroundValUshort; // unsigned short

  if (!bPrepareMaskOnly) // actual cropping is controled by
                         // checkBoxCropBkgroundCBCT. But mask files are always
                         // prepared.
  {
    auto input_fn = strPathRawCBCT;
    auto output_fn = strPathOutputCBCT;
    std::cout << "Entering plm_mask_main to crop the skin image." << std::endl;
    plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);
  } else {
    std::cout << "bPrepareMaskOnly flag is on. Skipping plm_mask_main.. "
              << std::endl;
    strPathOutputCBCT.clear();
  }

  m_strPathCTSkin_manRegi = strPath_mskSkinCT_manRegi; // for further use
  std::cout << "CBCT preprocessing is done! " << std::endl;

  // Delete temporary file (~450 MB)
  QFile::remove(strPath_outputXF_manualTrans);
}

// called after the auto rigid regi. 1) accurate skin clipping 2) air bubble
// filling inside of the CBCT
void CbctRegistration::ProcessCBCT_beforeDeformRegi(
    QString &strPathRawCBCT, QString &strPath_mskSkinCT_manRegi,
    QString &strPathOutputCBCT, QString &strPathXFAutoRigid,
    const bool bBubbleFilling, const bool bPrepareMaskOnly, double skinExp,
    const int bubbleThresh, const int bubbleFill) {
  if (m_pParent->m_spAutoRigidCT == nullptr) {
    return;
  }

  Warp_parms parms;
  Rt_study rtds;

  auto strPath_mskSkinCT_autoRegi =
      m_strPathPlastimatch + "/msk_skin_CT_autoRegi.mha";

  parms.input_fn = strPath_mskSkinCT_manRegi.toLocal8Bit().constData();
  parms.output_img_fn = strPath_mskSkinCT_autoRegi.toLocal8Bit().constData();
  parms.xf_in_fn = strPathXFAutoRigid.toLocal8Bit().constData();
  parms.fixed_img_fn = strPathRawCBCT.toLocal8Bit()
                           .constData(); // add this to fit Mask to CBCT image

  parms.prefix_format = "mha";
  parms.use_itk = 0;
  parms.interp_lin = 1; // linear
  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type = PLM_FILE_FMT_IMG;

  rt_study_warp(&rtds, file_type, &parms);
  printf("Skin mask based on auto regi is ready!\n");

  auto strPath_mskSkinCT_autoRegi_exp =
      m_strPathPlastimatch + "/msk_skin_CT_autoRegi_exp.mha";

  if (skinExp < 0.0 || skinExp > 100.0) {
    skinExp = 0.0;
  }

  plm_expansion_contract_msk(strPath_mskSkinCT_autoRegi,
                             strPath_mskSkinCT_autoRegi_exp,
                             skinExp); // 8 mm expansion for a mask image

  // 4) eliminate the air region (temporarily)
  // plastimatch mask --input E:\PlastimatchData\DicomEg\NEW\rawCBCT2.mha
  // --mask-value 0 --mask
  // E:\PlastimatchData\DicomEg\OLD\msk_skin_autoRegi_exp.mha --output
  // E:\PlastimatchData\DicomEg\NEW\rawCBCT4.mha

  // Mask_parms parms_msk;
  // QString strPath_CBCT_skinRemovedTemp = m_strPathPlastimatch +
  // "/skin_removed_CBCT_tmp.mha";

  auto mask_option = MASK_OPERATION_MASK;
  auto input_fn = strPathRawCBCT;
  auto mask_fn = strPath_mskSkinCT_autoRegi_exp;
  auto output_fn = strPathOutputCBCT;
  float mask_value = 0.0; // unsigned short

  if (!bPrepareMaskOnly) {
    plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);
  } else {
    strPathOutputCBCT = strPathRawCBCT;
  }

  m_strPathCTSkin_autoRegi =
      strPath_mskSkinCT_autoRegi; // for further use. this is not expanded one!

  //****************OPTIONAL
  if (!bBubbleFilling) {
    return; // exit
  }

  // Bubble filling
  //- With the latest air-removed image (rawCBCT4), find inside-body-bubbles.
  // plastimatch segment --input [rawCBCT.mha] --output-img
  // [msk_bubbles_CBCT.mha] --lower-threshold [air_thre_ushort]  plastimatch
  // segment --input E:\PlastimatchData\DicomEg\NEW\rawCBCT4.mha --output-img
  // E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT500.mha --lower-threshold
  // 500

  /*- fill skin regions in bubble mask image
        plastimatch mask --input
     E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT500.mha --mask-value 0
     --mask E:\PlastimatchData\DicomEg\OLD\msk_skin_autoRegi_cont.mha --output
     E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT_final.mha*/

  /*- (optional) fill lung regions
        plastimatch fill --input
     E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT_final.mha --mask-value 0
     --mask E:\PlastimatchData\DicomEg\OLD\msk_lungs_autoRegi_exp.mha --output
     E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT_final.mha*/

  auto strPathMskBubbleCBCT = m_strPathPlastimatch + "/msk_bubbles_CBCT.mha";
  const auto strPathMskBubbleCBCT_final =
      m_strPathPlastimatch + "/msk_bubbles_CBCT_final.mha";

  // int iAirThresholdUShort = 500; //depends on the CBCT image

  Plm_image in;
  Plm_image out;
  Segment_body sb;
  sb.m_lower_threshold = bubbleThresh;
  in.load_native(strPathOutputCBCT.toLocal8Bit().constData());

  sb.img_in = &in;
  sb.img_out = &out;
  /* Do segmentation */
  sb.do_segmentation_air_cavity();
  /* Save output file */
  sb.img_out->save_image(strPathMskBubbleCBCT.toLocal8Bit().constData());

  auto strPath_mskSkinCT_autoRegi_cont =
      m_strPathPlastimatch + "/msk_skin_CT_autoRegi_cont.mha";
  plm_expansion_contract_msk(strPath_mskSkinCT_autoRegi,
                             strPath_mskSkinCT_autoRegi_cont,
                             -10.0); //-10 mm expansion for a mask image

  // Mask_parms parms_msk_bubble;
  mask_option = MASK_OPERATION_MASK;
  input_fn = strPathMskBubbleCBCT;
  mask_fn = strPath_mskSkinCT_autoRegi_cont;
  output_fn = strPathMskBubbleCBCT_final;
  mask_value = 0.0; // unsigned short
  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

  // Fill CBCT bubbles with the above-bubble mask
  // plastimatch fill --input [air-removed_autoRegistered CBCT] --mask-value
  // [dummy CBCT soft-tissue value in ushort -->depending on the scan region?]
  // --mask [segmented air-bubble] --output [CBCT_final.mha --> holes and
  // outside(with margin) region-removed CBCT. ready for deformable
  // registration] plastimatch fill --input
  // E:\PlastimatchData\DicomEg\NEW\rawCBCT4.mha
  // --mask-value 700 --mask
  // E:\PlastimatchData\DicomEg\NEW\msk_bubbles_CBCT_final.mha --output
  // E:\PlastimatchData\DicomEg\NEW\rawCBCT_final.mha

  // Mask_parms parms_fill;
  auto strPathBubbleRemovedCBCT =
      m_strPathPlastimatch + "/bubble_filled_CBCT.mha"; // tmp
  // QString strPathBubbleRemovedCBCT = strPathOutputCBCT;  //OVERWRITTING

  mask_option = MASK_OPERATION_FILL;
  // parms_fill.input_fn = strPathRawCBCT.toLocal8Bit().constData();
  input_fn = strPathOutputCBCT; // CBCT after air correction
  mask_fn = strPathMskBubbleCBCT_final;
  // parms_fill.output_fn = strPathBubbleRemovedCBCT.toLocal8Bit().constData();
  output_fn = strPathOutputCBCT; // overwriting
  // parms_fill.mask_value = 700.0; //compromised softtissue value
  mask_value = static_cast<float>(bubbleFill); // compromised softtissue value
  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

  if (bBubbleFilling) { // if bubble was segmented
    m_strPathMskCBCTBubble =
        strPathMskBubbleCBCT_final; // save this for further use
  }
}

void CbctRegistration::CallingPLMCommand(std::string &command_filepath) {

  std::cout << "3: calling a plastimatch command" << std::endl;

  Registration reg;
  if (reg.set_command_file(command_filepath) != PLM_SUCCESS) {
    printf("Error.  could not load %s as command file.\n",
           command_filepath.c_str());
  }
  // std::cout << "command file path is set= " <<
  // pathCmdRegister.toLocal8Bit().constData() << std::endl;

  if (command_filepath.length() < 3) {
    std::cout << "ERROR! pathCmdRegister is too short!" << std::endl;
    return;
  }

  auto params = reg.get_registration_parms()->get_shared_parms();
  std::string strFixed;
  std::string strMoving;

  for (auto &it : params->metric) {
    if (strncmp(it.first.c_str(), "0", 1) != 0) {
      std::cout << it.first.c_str() << std::endl;
    }
    strFixed = it.second.fixed_fn; // fn is just File Name!!
    strMoving = it.second.moving_fn;
  }

  // std::string strFixed = reg.get_registration_parms()->get_fixed_fn(); //
  // return d_ptr->rparms;  std::string strMoving =
  // reg.get_registration_parms()->get_moving_fn();

  if (strFixed.length() < 1) {
    std::cout << std::endl;
    std::cout << std::endl;
    std::cout << std::endl;
    std::cout << "ERROR! no fixed image" << std::endl;
    // return;
  }
  if (strMoving.length() < 1) {
    std::cout << std::endl;
    std::cout << std::endl;
    std::cout << std::endl;
    std::cout << "ERROR!no moving image" << std::endl;
    // return;
  }

  reg.do_registration(); // error occurs here

  std::cout << "4: Registration is done" << std::endl;
}

DoubleVector3DType
CbctRegistration::CallingPLMCommandXForm(std::string &command_filepath) {

  std::cout << "3: calling a plastimatch command" << std::endl;

  Registration reg;
  if (reg.set_command_file(command_filepath) != PLM_SUCCESS) {
    printf("Error.  could not load %s as command file.\n",
           command_filepath.c_str());
  }
  // reg.get_registration_parms()->log_fn = "gradient_log.txt";
  reg.load_global_inputs();

  const auto xform = reg.do_registration_pure(); // changed from
                                                 // do_registration()
                                                 // without return value
  std::cout << "4: Registration is done" << std::endl;
  return xform->get_trn()->GetOffset();
}

bool CbctRegistration::CallingGPMCcommand(
    const enDevice device, const int n_sims, const int n_plans,
    QString &comma_sep_planfilepath, UShortImageType::Pointer &spFixed,
    UShortImageType::Pointer &spMoving, UShortImageType::Pointer &spFixedDose,
    UShortImageType::Pointer &spMovingDose) {

  auto gPMC_device = QString("cpu");
  switch (device) {
  case GPU_DEV:
    gPMC_device = QString("gpu");
    break;
  case CPU_DEV:
  default:
    break;
  }

  const auto image_independent_string =
      QString(" --hardware %1 --plan \"%2\" -n %3 -b %4 --verbose")
          .arg(gPMC_device)
          .arg(comma_sep_planfilepath)
          .arg(n_plans)
          .arg(n_sims);

  auto tmp_str = QString("tmp_");
  auto fix_str = QString("Fixed");
  auto mov_str = QString("Moving");
  QString moving_dcm_dir = "";
  // Export fixed and moving as DCM
  const auto fixed_dcm_dir = SaveUSHORTAsSHORT_DICOM_gdcmITK(
      spFixed, tmp_str, fix_str, m_strPathPlastimatch);
  if (spFixed != spMoving) {
    moving_dcm_dir = SaveUSHORTAsSHORT_DICOM_gdcmITK(spMoving, tmp_str, mov_str,
                                                     m_strPathPlastimatch);
  }

  auto gPMC_command_str =
      QString("gPMC.exe") + // casting this seems to cast the whole string
      " --dir \"" + fixed_dcm_dir + "\" --output \"" + fixed_dcm_dir +
      "/dose_fixed.mha\"" + get_output_options(spFixed) +
      image_independent_string;
  // std::cout << gPMC_command_str.toStdString() << std::endl;
  if (QProcess::execute(gPMC_command_str) < 0) {
    std::cerr << "Failed to run (fixed mc recalc)" << std::endl;
    return false;
  }

  if (!moving_dcm_dir.isEmpty()) {
    gPMC_command_str = QString("gPMC.exe") + " --dir " + moving_dcm_dir +
                       " --output " + moving_dcm_dir + "/dose_moving.mha" +
                       get_output_options(spMoving) + image_independent_string;

    if (QProcess::execute(gPMC_command_str) < 0) {
      std::cerr << "Failed to run (moving mc recalc)" << std::endl;
      return false;
    }
  }
  // Run gPMC externally ^

  // Translate gPMC output (preferably .mha) to ITK image
  using ImageReaderType = itk::ImageFileReader<FloatImageType>;
  using MinMaxFindType = itk::MinimumMaximumImageFilter<FloatImageType>;
  using MultiplyImageFilterType =
      itk::MultiplyImageFilter<FloatImageType, FloatImageType>;
  using CastFilterType = itk::CastImageFilter<FloatImageType, UShortImageType>;

  auto fixedDosePath = fixed_dcm_dir + "/dose_fixed.mha";
  auto finfofixedDosePath = QFileInfo(fixedDosePath);

  if (finfofixedDosePath.exists()) {
    auto FixedDoseReader = ImageReaderType::New();
    FixedDoseReader->SetFileName(fixedDosePath.toStdString());

    auto MinMaxFilter = MinMaxFindType::New();
    MinMaxFilter->SetInput(FixedDoseReader->GetOutput());
    MinMaxFilter->Update();
    // Multiply: Scale to USHORT
    auto multiplyImageFilter = MultiplyImageFilterType::New();
    multiplyImageFilter->SetInput(FixedDoseReader->GetOutput());
    multiplyImageFilter->SetConstant(65535.0f / MinMaxFilter->GetMaximum());

    auto castFilter = CastFilterType::New();
    castFilter->SetInput(multiplyImageFilter->GetOutput());

    castFilter->Update();
    spFixedDose = castFilter->GetOutput();

    if (spFixedDose == nullptr) {
      std::cout << "Dose failed to load for fixed Image!!" << std::endl;
    } else {
      std::cout << "Dose loaded for fixed Image" << std::endl;
    }

    auto imgDim = spFixedDose->GetBufferedRegion().GetSize();
    auto spacing = spFixedDose->GetSpacing();

    std::cout << "Image Dimension:	" << imgDim[0] << "	" << imgDim[1]
              << "	" << imgDim[2] << std::endl;
    std::cout << "Image Spacing (mm):	" << spacing[0] << "	" << spacing[1]
              << "	" << spacing[2] << std::endl;
  }

  if (!moving_dcm_dir.isEmpty()) {
    auto movingDosePath = moving_dcm_dir + "/dose_moving.mha";
    auto finfomovingDosePath = QFileInfo(movingDosePath);

    if (finfomovingDosePath.exists()) {
      auto MovingDoseReader = ImageReaderType::New();
      MovingDoseReader->SetFileName(movingDosePath.toStdString());

      auto MinMaxFilter = MinMaxFindType::New();
      MinMaxFilter->SetInput(MovingDoseReader->GetOutput());
      MinMaxFilter->Update();
      // Multiply: Scale to USHORT
      auto multiplyImageFilter = MultiplyImageFilterType::New();
      multiplyImageFilter->SetInput(MovingDoseReader->GetOutput());
      multiplyImageFilter->SetConstant(65535.0f / MinMaxFilter->GetMaximum());

      auto castFilter = CastFilterType::New();
      castFilter->SetInput(multiplyImageFilter->GetOutput());

      castFilter->Update();
      spMovingDose = castFilter->GetOutput();
      std::cout << "Dose loaded for moving Image" << std::endl;
    }
  } else {
    if (!spFixedDose.IsNull()) {
      spMovingDose = spFixedDose;
    }
  }
  // Display dose as colorwash on top of fixed and moving in all three plots
  if (spFixedDose == nullptr && spMovingDose == nullptr) {
    dose_loaded = false;
  } else {
    dose_loaded = true;
  }
  return dose_loaded;
}

void CbctRegistration::plm_synth_trans_xf(QString &strPath_fixed,
                                          QString &strPath_out_xf,
                                          const double transX,
                                          const double transY,
                                          const double transZ) const {
  Synthetic_vf_parms sv_parms;
  sv_parms.pattern = Synthetic_vf_parms::PATTERN_TRANSLATION;
  sv_parms.translation[0] = static_cast<float>(transX);
  sv_parms.translation[1] = static_cast<float>(transY);
  sv_parms.translation[2] = static_cast<float>(transZ);

  const auto fixed =
      itk_image_load_float(strPath_fixed.toLocal8Bit().constData(), nullptr);
  sv_parms.pih.set_from_itk_image(fixed);

  // Synthetic_vf_parms *sv_parms = &parms->sv_parms;

  const auto vf = synthetic_vf(&sv_parms);
  itk_image_save(vf, strPath_out_xf.toLocal8Bit().constData());
}

void CbctRegistration::SetPlmOutputDir(QString &endFix) {
  auto crntDir = QDir::current(); // folder where current exe file exists.
  auto crntPathStr = crntDir.absolutePath();
  const auto dirName = crntPathStr.append("/").append("plm_").append(endFix);

  auto tmpDir = QDir(dirName);
  if (!tmpDir.exists()) {
    if (!tmpDir.mkpath(dirName)) {
      std::cerr << "Could not create tmp dir for plm_ !!" << std::endl;
    }
  }
  m_strPathPlastimatch = dirName;
}

void CbctRegistration::PostSkinRemovingCBCT(
    UShortImageType::Pointer &spCBCT) const {
  if (spCBCT == nullptr) {
    std::cout << "Error! No CBCT image is available" << std::endl;
    return;
  }

  // find the closest skin contour for CBCT: !8 mm expansion from auto rigid
  // body

  QString strPath_mskSkinCT_final;
  const auto strPath_mskSkinCT_autoRegi_exp =
      m_strPathPlastimatch + "/msk_skin_CT_autoRegi_exp.mha";
  QFileInfo maskInfoAuto(strPath_mskSkinCT_autoRegi_exp);

  const auto strPath_mskSkinCT_manualRegi_exp =
      m_strPathPlastimatch + "/msk_skin_CT_manRegi_exp.mha";
  QFileInfo maskInfoManual(strPath_mskSkinCT_manualRegi_exp);

  if (maskInfoAuto.exists()) // if the mask file is not prepared, give up the
                             // skin removal
  {
    strPath_mskSkinCT_final = strPath_mskSkinCT_autoRegi_exp;
  } else {
    std::cout << "Mask file of auto-registration is not prepared. Use manual "
                 "regi-mask instead"
              << std::endl;

    if (maskInfoManual.exists()) {
      strPath_mskSkinCT_final = strPath_mskSkinCT_manualRegi_exp;
    } else {
      std::cout << "Mask file of manual registration is not prepared. Skip "
                   "skin removal!"
                << std::endl;
      return;
    }
  }

  // std::cout << "Plastimatch Path " <<
  // m_strPathPlastimatch.toLocal8Bit().constData() << std::endl;

  if (m_strPathPlastimatch.length() < 1) {
    std::cout << "NO plastimatch Dir was defined. CorrCBCT will not be saved "
                 "automatically"
              << std::endl;
    return;
  }
  // 1) Export current CBCT file
  auto filePathCBCT =
      m_strPathPlastimatch + "/" + "CorrCBCT.mha"; // usually corrected one
  auto filePathCBCT_noSkin = m_strPathPlastimatch + "/" +
                             "CorrCBCT_final.mha"; // usually corrected one

  using writerType = itk::ImageFileWriter<UShortImageType>;
  auto writer = writerType::New();
  writer->SetFileName(filePathCBCT.toLocal8Bit().constData());
  writer->SetUseCompression(true);
  writer->SetInput(spCBCT);

  std::cout << "Writing the CBCT file" << std::endl;
  writer->Update();

  QFileInfo CBCTInfo(filePathCBCT);
  if (!CBCTInfo.exists()) {
    std::cout << "No CBCT file to read. Maybe prior writing failed"
              << std::endl;
    return;
  }

  // ERROR HERE! delete the temporry folder.
  std::cout << "Delete the temporary folder if it crashes" << std::endl;

  // 4) eliminate the air region (temporarily)
  // Mask_parms parms_msk;
  // DIMENSION SHOULD BE MATCHED!!!! BETWEEN raw CBCT and Mask files
  const auto mask_option = MASK_OPERATION_MASK;
  QString input_fn = filePathCBCT.toLocal8Bit().constData();
  QString mask_fn = strPath_mskSkinCT_final.toLocal8Bit().constData();
  QString output_fn = filePathCBCT_noSkin.toLocal8Bit().constData();
  const float mask_value = 0.0; // unsigned short
  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

  // m_strPathCTSkin_autoRegi = strPath_mskSkinCT_autoRegi; //for further use.
  // this is not expanded one!

  using readerType = itk::ImageFileReader<UShortImageType>;
  auto readerCBCT = readerType::New();
  QFileInfo tmpFileInfo(filePathCBCT_noSkin);

  if (tmpFileInfo.exists()) {
    readerCBCT->SetFileName(filePathCBCT_noSkin.toLocal8Bit().constData());
    std::cout << "Reading the corrected file" << std::endl;
    readerCBCT->Update();
    spCBCT = readerCBCT->GetOutput();
  } else {
    std::cout << "Error! No skin-removed file is available for reading"
              << std::endl;
  }
}

void CbctRegistration::ThermoMaskRemovingCBCT(
    UShortImageType::Pointer &spCBCTraw, UShortImageType::Pointer &spCBCTcor,
    const int diffThreshold, const int noTouchThreshold,
    const double innerMargin, const double outerMargin) const {
  if (spCBCTraw == nullptr || spCBCTcor == nullptr) {
    std::cout << "You need both raw and corr CBCT images" << std::endl;
    return;
  }

  QString strPathInputMask;

  const auto strPath_mskSkinCT_manRegi =
      m_strPathPlastimatch + "/msk_skin_CT_manRegi.mha";
  const auto strPath_mskSkinCT_autoRegi =
      m_strPathPlastimatch + "/msk_skin_CT_autoRegi.mha";

  QFileInfo maskInfoMan(strPath_mskSkinCT_manRegi);
  QFileInfo maskInfoAuto(strPath_mskSkinCT_autoRegi);

  if (maskInfoAuto.exists()) {
    strPathInputMask = strPath_mskSkinCT_autoRegi;
  } else if (maskInfoMan.exists()) {
    strPathInputMask = strPath_mskSkinCT_manRegi;
  } else {
    std::cout << "Error! no available mask exist" << std::endl;
    return;
  }
  auto strPathOutputMask = m_strPathPlastimatch + "/msk_skin_CT_shell.mha";
  GenShellMask(strPathInputMask, strPathOutputMask, innerMargin, outerMargin);

  // Load shell mask

  QFileInfo fInfoOutput(strPathOutputMask);

  if (!fInfoOutput.exists()) {
    std::cout << "error! GenShellMask DIDN'T WORK WELL" << std::endl;
    return;
  }

  using readerType = itk::ImageFileReader<UShortImageType>;
  auto reader = readerType::New();
  reader->SetFileName(strPathOutputMask.toLocal8Bit().constData());
  reader->Update();

  const UShortImageType::Pointer spShellMask = reader->GetOutput();

  itk::ImageRegionIterator<UShortImageType> itRaw(
      spCBCTraw, spCBCTraw->GetBufferedRegion());
  itk::ImageRegionIterator<UShortImageType> itCor(
      spCBCTcor, spCBCTcor->GetBufferedRegion());
  itk::ImageRegionIterator<UShortImageType> itMask(
      spShellMask, spShellMask->GetBufferedRegion());

  auto size1 = spCBCTraw->GetBufferedRegion().GetSize();
  // UShortImageType::SizeType size2 = spCBCTcor->GetBufferedRegion().GetSize();
  auto size3 = spShellMask->GetBufferedRegion().GetSize();

  if (size1[0] != size3[0] || size1[1] != size3[1] || size1[2] != size3[2]) {
    std::cout << "Error! size is different."
              << "  " << size3 << std::endl;
    return;
  }

  for (itRaw.GoToBegin(), itCor.GoToBegin(), itMask.GoToBegin();
       !itRaw.IsAtEnd() && !itCor.IsAtEnd() && !itMask.IsAtEnd();
       ++itRaw, ++itCor, ++itMask) {
    if (itMask.Get() > 0) // in shell
    {
      const auto diffHU = itCor.Get() - itRaw.Get();
      // do not change the HU value if corrCBCT is at reasonable value
      if (diffHU > diffThreshold && itCor.Get() > noTouchThreshold) {
        itCor.Set(itRaw.Get());
      }
    }
  }
}

void CbctRegistration::GenShellMask(
    QString &strPathInputMask, QString &strPathOutputMask,
    const double fInnerMargin,
    const double fOuterMargin) const { // writes mask to disk
  QFileInfo fInfoInput(strPathInputMask);

  if (!fInfoInput.exists()) {
    return;
  }

  auto strPathTmpExp = fInfoInput.absolutePath() + "/" + "/msk_temp_exp.mha";
  auto strPathTmpCont = fInfoInput.absolutePath() + "/" + "/msk_temp_cont.mha";
  plm_expansion_contract_msk(strPathInputMask, strPathTmpExp,
                             fOuterMargin); // 8 mm expansion for a mask image
  plm_expansion_contract_msk(strPathInputMask, strPathTmpCont,
                             -fInnerMargin); // 8 mm expansion for a mask image

  // Mask_operation mask_option = MASK_OPERATION_MASK;
  const auto mask_option = MASK_OPERATION_FILL;
  auto input_fn = strPathTmpExp;
  auto mask_fn = strPathTmpCont;
  auto output_fn = strPathOutputMask;
  const float mask_value = 0.0; // unsigned short

  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);

  QFile::remove(strPathTmpExp);
  QFile::remove(strPathTmpCont);
}

void CbctRegistration::CropSkinUsingRS(UShortImageType::Pointer &spImgUshort,
                                       QString &strPathRS,
                                       const double cropMargin) const {
  if (fabs(cropMargin) > 0.01) {
    std::cout << "margin has not been implemented yet. regarded as 0.0 in this "
                 "version"
              << std::endl;
  }
  if (spImgUshort == nullptr) {
    return;
  }

  /* Load RS file to make a Skin mask*/
  Warp_parms parms;
  Rt_study rtds;

  // Export cur image first
  auto filePathCurImg = m_strPathPlastimatch + "/" +
                        "SkinCropRS_curImg.mha"; // usually corrected one

  using writerType = itk::ImageFileWriter<UShortImageType>;

  auto writer = writerType::New();
  writer->SetFileName(filePathCurImg.toLocal8Bit().constData());
  writer->SetUseCompression(true);
  writer->SetInput(spImgUshort);
  std::cout << "Writing the current image file" << std::endl;
  writer->Update();

  parms.input_fn = strPathRS.toLocal8Bit().constData();
  parms.fixed_img_fn = filePathCurImg.toLocal8Bit().constData();

  auto ssimg_path_all = m_strPathPlastimatch + "/ssimg_all_cstm.mha";
  auto sslist_path_all = m_strPathPlastimatch + "/sslist_all_cstm.txt";
  parms.output_ss_img_fn = ssimg_path_all.toLocal8Bit().constData();
  parms.output_ss_list_fn = sslist_path_all.toLocal8Bit().constData();

  parms.prefix_format = "mha";
  parms.use_itk = 0;
  parms.interp_lin = 1;

  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type = PLM_FILE_FMT_DICOM_RTSS;
  /*if (file_type == PLM_FILE_FMT_POINTSET) {
  warp_pointset_main (&parms);
  return;
  }*/
  /* Process warp */
  rt_study_warp(&rtds, file_type, &parms);
  printf("Warping Finished!\n");

  // return;

  /* [3]Read outputss-list.txt and leave skin only*/
  std::ifstream fin;
  fin.open(sslist_path_all.toLocal8Bit().constData(), std::ios::in);
  if (fin.fail()) {
    return;
  }

  char str[MAX_LINE_LENGTH];

  QString strLineSkin;

  while (!fin.eof()) {
    memset(str, 0, MAX_LINE_LENGTH);
    fin.getline(str, MAX_LINE_LENGTH);
    QString strLine(str);

    auto strList = strLine.split('|');
    // third one is the organ name
    if (strList.length() != 3) {
      std::cout << "abnormal file expression." << std::endl;
      break;
    }
    auto organName = strList.at(2);

    organName = organName.trimmed();
    if (organName == "Skin" || organName == "skin" || organName == "SKIN") {
      strLineSkin = strLine;
    }
    /*if (organName == "LeftLung")
    {
    strLineLungLt = strLine;
    }
    if (organName == "RightLung")
    {
    strLineLungRt = strLine;
    }*/
  }
  fin.close();

  QString sslist_path_skin;
  if (strLineSkin.length() > 1) {
    std::ofstream fout;
    sslist_path_skin = sslist_path_all;
    sslist_path_skin.replace("all", "skin");
    fout.open(sslist_path_skin.toLocal8Bit().constData());

    fout << strLineSkin.toLocal8Bit().constData() << std::endl;

    fout.close();
  } else {
    std::cout << "Error: no skin contour is found in DICOM RS file. "
              << std::endl;
    return;
  }
  /* End of [3]Read outputss-list.txt and leave skin only*/

  /* [4]prepare a skin mask image*/
  // plastimatch convert --input-ss-img [ssimg_all.mha] --input-ss-list
  // [sslist_skin.txt] --output-labelmap [msk_skin.mha]

  Warp_parms parms2;
  Rt_study rtds2;
  // convert --input-ss-img E:\PlastimatchData\DicomEg\OLD\ssimg_all.mha
  // --input-ss-list E:\PlastimatchData\DicomEg\OLD\sslist_skin.txt
  // --output-labelmap E:\PlastimatchData\DicomEg\OLD\msk_skin.mha
  auto strPath_mskSkinCT = m_strPathPlastimatch + "/msk_skin_CT_cstm.mha";
  parms2.input_ss_img_fn = ssimg_path_all.toLocal8Bit().constData();
  parms2.input_ss_list_fn = sslist_path_skin.toLocal8Bit().constData();
  parms2.output_labelmap_fn =
      strPath_mskSkinCT.toLocal8Bit().constData(); // output
  parms2.prefix_format = "mha";
  parms2.use_itk = 0;
  parms2.interp_lin = 1;
  // file_type = plm_file_format_deduce ((const char*) parms.input_fn);
  const auto file_type2 = PLM_FILE_FMT_NO_FILE;
  /* Process warp */
  rt_study_warp(&rtds2, file_type2, &parms2);
  printf("Warping2 Finished!\n");
  // m_strPathCTSkin = strPath_mskSkinCT;

  // Mask_parms parms_msk3;
  auto strPathSkinRemovedCT =
      m_strPathPlastimatch + "/skin_removed_CT_cstm.mha";
  const auto mask_option = MASK_OPERATION_MASK;
  QFileInfo tmpInfo(filePathCurImg);

  QString input_fn;
  if (tmpInfo.exists()) {
    input_fn = filePathCurImg;
  } else {
    return;
  }

  auto mask_fn = strPath_mskSkinCT;
  auto output_fn = strPathSkinRemovedCT;
  const auto mask_value = 0.0f;
  plm_mask_main(mask_option, input_fn, mask_fn, output_fn, mask_value);
  // strPathSkinRemovedCT .mha file is ready. this is SHORT image

  using readerType = itk::ImageFileReader<UShortImageType>;
  auto reader = readerType::New();

  auto tmpFileInfo = QFileInfo(strPathSkinRemovedCT); // cropped image
  if (tmpFileInfo.exists()) {
    reader->SetFileName(strPathSkinRemovedCT.toLocal8Bit().constData());
    reader->Update();

    spImgUshort = reader->GetOutput();
    // std::cout << "fixed Image Path = " <<
    // filePathFixed_proc.toLocal8Bit().constData() << std::endl;
  } else {
    std::cout << "No strPathSkinRemovedCT is available. Exit the function"
              << std::endl;
  }
}

VEC3D CbctRegistration::GetShiftValueFromGradientXForm(QString &file_path,
                                                       const bool b_inverse) {

  auto res_val = VEC3D{0.0, 0.0, 0.0};

  std::ifstream fin;
  fin.open(file_path.toLocal8Bit().constData());

  if (fin.fail()) {
    return res_val;
  }

  char str[MAX_LINE_LENGTH];
  // memset(str, 0, MAX_LINE_LENGTH);

  QString tmp_str;
  while (!fin.eof()) {
    memset(str, 0, MAX_LINE_LENGTH);
    fin.getline(str, MAX_LINE_LENGTH);
    tmp_str = QString(str);

    if (tmp_str.contains("Parameters") && !tmp_str.contains("#")) {
      break;
    }
  }
  auto str_list = tmp_str.split(" ");
  if (str_list.count() < 4) {
    std::cout << "error: not enough shift information" << std::endl;
    return res_val;
  }

  res_val.x = str_list.at(1).toDouble();
  res_val.y = str_list.at(2).toDouble();
  res_val.z = str_list.at(3).toDouble();

  if (b_inverse) {
    res_val.x = -res_val.x;
    res_val.y = -res_val.y;
    res_val.z = -res_val.z;
  }

  fin.close();

  return res_val;

  //#Insight Transform File V1.0
  //#Transform 0
  // Transform: TranslationTransform_double_3_3
  // Parameters: 1.407247543334961 45.48719024658203 -125.87542724609375
  //         FixedParameters :
  //
}

VEC3D CbctRegistration::GetIsocenterDCM_FromRTPlan(QString &strFilePath) const {
  auto resultPtDcm = VEC3D{0.0, 0.0, 0.0};
  auto p_rt_study_rp = std::make_unique<Dcmtk_rt_study>();

  // std::cout << "Before plm_file_format_deduce" << std::endl;
  const auto file_type_dcm_plan =
      plm_file_format_deduce(strFilePath.toLocal8Bit().constData());
  // std::cout << "After plm_file_format_deduce" << std::endl;

  if (file_type_dcm_plan == PLM_FILE_FMT_DICOM_RTPLAN) {
    std::cout << "PLM_FILE_FMT_DICOM_RTPLAN "
              << "is found" << std::endl;
    p_rt_study_rp->load(strFilePath.toLocal8Bit().constData());
  } else {
    std::cout << "Found file is not RTPLAN. Skipping dcm plan." << std::endl;
    return resultPtDcm;
  }
  auto rtplan = p_rt_study_rp->get_rtplan();

  if (!rtplan) {
    std::cout << "Error! no dcm plan is loaded" << std::endl;
    return resultPtDcm;
  }

  const auto iCntBeam = rtplan->beamlist.size(); // num_beams;

  if (iCntBeam < 1) {
    std::cout << "Error! no beam is found" << std::endl;
    return resultPtDcm;
  }

  float *final_iso_pos = nullptr;

  for (size_t i = 0; i < iCntBeam; i++) {
    auto *cur_beam = rtplan->beamlist[i];

    const auto iCntCP = cur_beam->cplist.size(); // num_cp;

    for (size_t j = 0; j < iCntCP; j++) {
      const auto cur_iso_pos = cur_beam->cplist[j]->get_isocenter();
      //                ID                id                               ID
      std::cout << "Beam Gantry: " << cur_beam->gantry_angle
                << ", Control point rate: "
                << cur_beam->cplist[j]->meterset_rate << // control_pt_no <<
          ", Isocenter pos : " << cur_iso_pos[0] << "/" << cur_iso_pos[1] << "/"
                << cur_iso_pos[2] << std::endl;

      if (i == 0 && j == 0) { // choose first beam's isocenter
        final_iso_pos = cur_beam->cplist[j]->get_isocenter();
      }
    }
  }
  // VEC3D shiftVal;// = GetShiftValueFromGradientXForm(filePathXform, true);
  // //true: inverse trans should be applied if CBCT was moving image //in mm

  if (final_iso_pos == nullptr) {
    std::cout << "Error!  No isocenter position was found. " << std::endl;
    return resultPtDcm;
  }

  resultPtDcm.x = static_cast<double>(final_iso_pos[0]);
  resultPtDcm.y = static_cast<double>(final_iso_pos[1]);
  resultPtDcm.z = static_cast<double>(final_iso_pos[2]);

  return resultPtDcm;
}
