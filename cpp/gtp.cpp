#include "core/global.h"
#include "core/config_parser.h"
#include "core/timer.h"
#include "search/asyncbot.h"
#include "program/setup.h"
#include "program/play.h"
#include "main.h"

using namespace std;

#define TCLAP_NAMESTARTSTRING "-" //Use single dashes for all flags
#include <tclap/CmdLine.h>

static bool tryParsePlayer(const string& s, Player& pla) {
  string str = Global::toLower(s);
  if(str == "black" || str == "b") {
    pla = P_BLACK;
    return true;
  }
  else if(str == "white" || str == "w") {
    pla = P_WHITE;
    return true;
  }
  return false;
}

static bool tryParseLoc(const string& s, const Board& b, Loc& loc) {
  return Location::tryOfString(s,b,loc);
}

static int numHandicapStones(const BoardHistory& hist) {
  const Board board = hist.initialBoard;
  int startBoardNumBlackStones = 0;
  int startBoardNumWhiteStones = 0;
  for(int y = 0; y<board.y_size; y++) {
    for(int x = 0; x<board.x_size; x++) {
      Loc loc = Location::getLoc(x,y,board.x_size);
      if(board.colors[loc] == C_BLACK)
        startBoardNumBlackStones += 1;
      else if(board.colors[loc] == C_WHITE)
        startBoardNumWhiteStones += 1;
    }
  }
  if(startBoardNumWhiteStones == 0)
    return startBoardNumBlackStones;
  return 0;
}


int MainCmds::gtp(int argc, const char* const* argv) {
  Board::initHash();
  ScoreValue::initTables();
  Rand seedRand;

  string configFile;
  string nnModelFile;
  string overrideVersion;
  try {
    TCLAP::CmdLine cmd("Run GTP engine", ' ', "1.0",true);
    TCLAP::ValueArg<string> configFileArg("","config","Config file to use (see configs/gtp_example.cfg)",true,string(),"FILE");
    TCLAP::ValueArg<string> nnModelFileArg("","model","Neural net model file",true,string(),"FILE");
    TCLAP::ValueArg<string> overrideVersionArg("","override-version","Force KataGo to say a certain value in response to gtp version command",false,string(),"VERSION");
    cmd.add(configFileArg);
    cmd.add(nnModelFileArg);
    cmd.add(overrideVersionArg);
    cmd.parse(argc,argv);
    configFile = configFileArg.getValue();
    nnModelFile = nnModelFileArg.getValue();
    overrideVersion = overrideVersionArg.getValue();
  }
  catch (TCLAP::ArgException &e) {
    cerr << "Error: " << e.error() << " for argument " << e.argId() << endl;
    return 1;
  }

  ConfigParser cfg(configFile);

  Logger logger;
  logger.addFile(cfg.getString("logFile"));
  bool logAllGTPCommunication = cfg.getBool("logAllGTPCommunication");
  bool logSearchInfo = cfg.getBool("logSearchInfo");

  if(cfg.contains("logToStderr") && cfg.getBool("logToStderr"))
    logger.setLogToStderr(true);

  logger.write("GTP Engine starting...");

  Rules initialRules;
  {
    string koRule = cfg.getString("koRule", Rules::koRuleStrings());
    string scoringRule = cfg.getString("scoringRule", Rules::scoringRuleStrings());
    bool multiStoneSuicideLegal = cfg.getBool("multiStoneSuicideLegal");
    float komi = 7.5f; //Default komi, gtp will generally override this

    initialRules.koRule = Rules::parseKoRule(koRule);
    initialRules.scoringRule = Rules::parseScoringRule(scoringRule);
    initialRules.multiStoneSuicideLegal = multiStoneSuicideLegal;
    initialRules.komi = komi;
  }

  SearchParams params;
  {
    vector<SearchParams> paramss = Setup::loadParams(cfg);
    if(paramss.size() != 1)
      throw StringError("Can only specify examply one search bot in gtp mode");
    params = paramss[0];
  }

  const bool ponderingEnabled = cfg.getBool("ponderingEnabled");
  const bool cleanupBeforePass = cfg.contains("cleanupBeforePass") ? cfg.getBool("cleanupBeforePass") : false;
  const bool allowResignation = cfg.contains("allowResignation") ? cfg.getBool("allowResignation") : false;
  const double resignThreshold = cfg.contains("allowResignation") ? cfg.getDouble("resignThreshold",-1.0,0.0) : -1.0; //Threshold on [-1,1], regardless of winLossUtilityFactor
  const int whiteBonusPerHandicapStone = cfg.contains("whiteBonusPerHandicapStone") ? cfg.getInt("whiteBonusPerHandicapStone",0,1) : 0;

  NNEvaluator* nnEval = NULL;
  AsyncBot* bot = NULL;

  Setup::initializeSession(cfg);

  auto maybeInitializeNNEvalAndAsyncBot = [&nnEval,&bot,&cfg,&params,&nnModelFile,&logger,&seedRand](int boardSize) {
    if(nnEval != NULL && boardSize == nnEval->getPosLen())
      return;
    if(nnEval != NULL) {
      assert(bot != NULL);
      bot->stopAndWait();
      delete bot;
      delete nnEval;
      bot = NULL;
      nnEval = NULL;
      logger.write("Cleaned up old neural net and bot");
    }

    int maxConcurrentEvals = params.numThreads * 2 + 16; // * 2 + 16 just to give plenty of headroom
    vector<NNEvaluator*> nnEvals = Setup::initializeNNEvaluators({nnModelFile},{nnModelFile},cfg,logger,seedRand,maxConcurrentEvals,false,false,boardSize);
    assert(nnEvals.size() == 1);
    nnEval = nnEvals[0];
    logger.write("Loaded neural net with posLen " + Global::intToString(nnEval->getPosLen()));

    string searchRandSeed;
    if(cfg.contains("searchRandSeed"))
      searchRandSeed = cfg.getString("searchRandSeed");
    else
      searchRandSeed = Global::uint64ToString(seedRand.nextUInt64());

    bot = new AsyncBot(params, nnEval, &logger, searchRandSeed);
  };

  maybeInitializeNNEvalAndAsyncBot(19);

  {
    Board board(19,19);
    Player pla = P_BLACK;
    BoardHistory hist(board,pla,initialRules,0);
    bot->setPosition(pla,board,hist);
  }

  TimeControls bTimeControls;
  TimeControls wTimeControls;

  vector<double> recentWinLossValues;
  const double searchFactorWhenWinning = cfg.contains("searchFactorWhenWinning") ? cfg.getDouble("searchFactorWhenWinning",0.01,1.0) : 1.0;
  const double searchFactorWhenWinningThreshold = cfg.contains("searchFactorWhenWinningThreshold") ? cfg.getDouble("searchFactorWhenWinningThreshold",0.0,1.0) : 1.0;
  double lastSearchFactor = 1.0;

  //Check for unused config keys
  cfg.warnUnusedKeys(cerr,&logger);

  //Komi without whiteBonusPerHandicapStone hack
  float unhackedKomi = bot->getRootHist().rules.komi;
  auto updateKomiIfNew = [&bot,&unhackedKomi,&whiteBonusPerHandicapStone,&recentWinLossValues]() {
    float newKomi = unhackedKomi;
    newKomi += numHandicapStones(bot->getRootHist()) * whiteBonusPerHandicapStone;
    if(newKomi != bot->getRootHist().rules.komi)
      recentWinLossValues.clear();
    bot->setKomiIfNew(newKomi);
  };

  bool currentlyAnalyzing = false;

  vector<string> knownCommands = {
    "protocol_version",
    "name",
    "version",
    "known_command",
    "list_commands",
    "quit",
    "boardsize",
    "clear_board",
    "komi",
    "play",
    "genmove",
    "showboard",
    "place_free_handicap",
    "set_free_handicap",
    "time_settings",
    "time_left",
    "final_score",
    "final_status_list",
    "lz-analyze",
    "kata-analyze",
    "stop",
  };

  logger.write("Beginning main protocol loop");

  string line;
  while(cin) {
    getline(cin,line);

    //Parse command, extracting out the command itself, the arguments, and any GTP id number for the command.
    string command;
    vector<string> pieces;
    bool hasId = false;
    int id = 0;
    {
      //Filter down to only "normal" ascii characters. Also excludes carrage returns.
      //Newlines are already handled by getline
      size_t newLen = 0;
      for(size_t i = 0; i < line.length(); i++)
        if(((int)line[i] >= 32 && (int)line[i] <= 126) || line[i] == '\t')
          line[newLen++] = line[i];

      line.erase(line.begin()+newLen, line.end());

      //Remove comments
      size_t commentPos = line.find("#");
      if(commentPos != string::npos)
        line = line.substr(0, commentPos);

      //Convert tabs to spaces
      for(size_t i = 0; i < line.length(); i++)
        if(line[i] == '\t')
          line[i] = ' ';

      line = Global::trim(line);
      if(line.length() == 0)
        continue;

      assert(line.length() > 0);

      if(logAllGTPCommunication)
        logger.write("Controller: " + line);

      //Parse id number of command, if present
      size_t digitPrefixLen = 0;
      while(digitPrefixLen < line.length() && Global::isDigit(line[digitPrefixLen]))
        digitPrefixLen++;
      if(digitPrefixLen > 0) {
        hasId = true;
        try {
          id = Global::parseDigits(line,0,digitPrefixLen);
        }
        catch(const IOError& e) {
          cout << "? GTP id '" << id << "' could not be parsed: " << e.what() << endl;
          continue;
        }
        line = line.substr(digitPrefixLen);
      }

      line = Global::trim(line);
      if(line.length() <= 0) {
        cout << "? empty command" << endl;
        continue;
      }

      pieces = Global::split(line,' ');
      for(size_t i = 0; i<pieces.size(); i++)
        pieces[i] = Global::trim(pieces[i]);
      assert(pieces.size() > 0);

      command = pieces[0];
      pieces.erase(pieces.begin());
    }

    //Upon any command, stop any analysis and output a newline
    if(currentlyAnalyzing) {
      bot->stopAndWait();
      cout << endl;
    }

    bool responseIsError = false;
    bool shouldQuitAfterResponse = false;
    bool maybeStartPondering = false;
    string response;

    if(command == "protocol_version") {
      response = "2";
    }

    else if(command == "name") {
      if (overrideVersion.size() > 0)
        response = "Leela Zero";
      else
        response = "KataGo";
    }

    else if(command == "version") {
      if(overrideVersion.size() > 0)
        response = overrideVersion;
      else
        response = "1.1";
    }

    else if(command == "known_command") {
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected single argument for known_command but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        if(std::find(knownCommands.begin(), knownCommands.end(), pieces[0]) != knownCommands.end())
          response = "true";
        else
          response = "false";
      }
    }

    else if(command == "list_commands") {
      for(size_t i = 0; i<knownCommands.size(); i++)
        response += knownCommands[i] + "\n";
    }

    else if(command == "quit") {
      shouldQuitAfterResponse = true;
      logger.write("Quit requested by controller");
    }

    else if(command == "boardsize") {
      int newBSize = 0;
      if(pieces.size() != 1 || !Global::tryStringToInt(pieces[0],newBSize)) {
        responseIsError = true;
        response = "Expected single int argument for boardsize but got '" + Global::concat(pieces," ") + "'";
      }
      else if(newBSize < 2 || newBSize > Board::MAX_LEN) {
        responseIsError = true;
        response = "unacceptable size";
      }
      else if(newBSize > Board::MAX_LEN) {
        responseIsError = true;
        response = Global::strprintf("unacceptable size (Board::MAX_LEN is %d, consider increasing and recompiling)",(int)Board::MAX_LEN);
      }
      else {
        maybeInitializeNNEvalAndAsyncBot(newBSize);
        Board board(newBSize,newBSize);
        Player pla = P_BLACK;
        BoardHistory hist(board,pla,bot->getRootHist().rules,0);
        bot->setPosition(pla,board,hist);
        updateKomiIfNew();
        recentWinLossValues.clear();
      }
    }

    else if(command == "clear_board") {
      assert(bot->getRootBoard().x_size == bot->getRootBoard().y_size);
      int newBSize = bot->getRootBoard().x_size;
      Board board(newBSize,newBSize);
      Player pla = P_BLACK;
      BoardHistory hist(board,pla,bot->getRootHist().rules,0);
      bot->setPosition(pla,board,hist);
      updateKomiIfNew();
      recentWinLossValues.clear();
    }

    else if(command == "komi") {
      float newKomi = 0;
      if(pieces.size() != 1 || !Global::tryStringToFloat(pieces[0],newKomi)) {
        responseIsError = true;
        response = "Expected single float argument for komi but got '" + Global::concat(pieces," ") + "'";
      }
      //GTP spec says that we should accept any komi, but we're going to ignore that.
      else if(isnan(newKomi) || newKomi < -100.0 || newKomi > 100.0) {
        responseIsError = true;
        response = "unacceptable komi";
      }
      else if(!Rules::komiIsIntOrHalfInt(newKomi)) {
        responseIsError = true;
        response = "komi must be an integer or half-integer";
      }
      else {
        unhackedKomi = newKomi;
        updateKomiIfNew();
        //In case the controller tells us komi every move, restart pondering afterward.
        maybeStartPondering = bot->getRootHist().moveHistory.size() > 0;
      }
    }

    else if(command == "time_settings") {
      double mainTime;
      double byoYomiTime;
      int byoYomiStones;
      if(pieces.size() != 3
         || !Global::tryStringToDouble(pieces[0],mainTime)
         || !Global::tryStringToDouble(pieces[1],byoYomiTime)
         || !Global::tryStringToInt(pieces[2],byoYomiStones)
         ) {
        responseIsError = true;
        response = "Expected 2 floats and an int for time_settings but got '" + Global::concat(pieces," ") + "'";
      }
      else if(isnan(mainTime) || mainTime < 0.0 || mainTime > 1e50) {
        responseIsError = true;
        response = "invalid main_time";
      }
      else if(isnan(byoYomiTime) || byoYomiTime < 0.0 || byoYomiTime > 1e50) {
        responseIsError = true;
        response = "invalid byo_yomi_time";
      }
      else if(byoYomiStones < 0 || byoYomiStones > 100000) {
        responseIsError = true;
        response = "invalid byo_yomi_stones";
      }
      else {
        TimeControls tc;
        //This means no time limits, according to gtp spec
        if(byoYomiStones == 0 && byoYomiTime > 0.0) {
          //do nothing, tc already no limits by default
        }
        //Absolute time
        else if(byoYomiStones == 0) {
          tc.originalMainTime = mainTime;
          tc.increment = 0.0;
          tc.originalNumPeriods = 0;
          tc.numStonesPerPeriod = 0;
          tc.perPeriodTime = 0.0;
          tc.mainTimeLeft = mainTime;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = 0;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }
        else {
          tc.originalMainTime = mainTime;
          tc.increment = 0.0;
          tc.originalNumPeriods = 1;
          tc.numStonesPerPeriod = byoYomiStones;
          tc.perPeriodTime = byoYomiTime;
          tc.mainTimeLeft = mainTime;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = 1;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }

        bTimeControls = tc;
        wTimeControls = tc;
      }
    }

    else if(command == "time_left") {
      Player pla;
      double time;
      int stones;
      if(pieces.size() != 3
         || !tryParsePlayer(pieces[0],pla)
         || !Global::tryStringToDouble(pieces[1],time)
         || !Global::tryStringToInt(pieces[2],stones)
         ) {
        responseIsError = true;
        response = "Expected player and float time and int stones for time_left but got '" + Global::concat(pieces," ") + "'";
      }
      //Be slightly tolerant of negative time left
      else if(isnan(time) || time < -10.0 || time > 1e50) {
        responseIsError = true;
        response = "invalid time";
      }
      else if(stones < 0 || stones > 100000) {
        responseIsError = true;
        response = "invalid stones";
      }
      else {
        TimeControls tc = pla == P_BLACK ? bTimeControls : wTimeControls;
        //Main time
        if(stones == 0) {
          tc.mainTimeLeft = time;
          tc.inOvertime = false;
          tc.numPeriodsLeftIncludingCurrent = tc.originalNumPeriods;
          tc.numStonesLeftInPeriod = 0;
          tc.timeLeftInPeriod = 0;
        }
        else {
          tc.mainTimeLeft = 0.9;
          tc.inOvertime = true;
          tc.numPeriodsLeftIncludingCurrent = 1;
          tc.numStonesLeftInPeriod = stones;
          tc.timeLeftInPeriod = time;
        }
        if(pla == P_BLACK)
          bTimeControls = tc;
        else
          wTimeControls = tc;
      }
    }

    else if(command == "play") {
      Player pla;
      Loc loc;
      if(pieces.size() != 2) {
        responseIsError = true;
        response = "Expected two arguments for play but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!tryParsePlayer(pieces[0],pla)) {
        responseIsError = true;
        response = "Could not parse color: '" + pieces[0] + "'";
      }
      else if(!tryParseLoc(pieces[1],bot->getRootBoard(),loc)) {
        responseIsError = true;
        response = "Could not parse vertex: '" + pieces[1] + "'";
      }
      else {
        bool suc = bot->makeMove(loc,pla);
        if(!suc) {
          responseIsError = true;
          response = "illegal move";
        }
        maybeStartPondering = true;
      }
    }

    else if(command == "genmove") {
      Player pla;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for genmove but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!tryParsePlayer(pieces[0],pla)) {
        responseIsError = true;
        response = "Could not parse color: '" + pieces[0] + "'";
      }
      else {
        ClockTimer timer;
        nnEval->clearStats();
        TimeControls tc = pla == P_BLACK ? bTimeControls : wTimeControls;

        //Play faster when winning
        double searchFactor = Play::getSearchFactor(searchFactorWhenWinningThreshold,searchFactorWhenWinning,params,recentWinLossValues,pla);
        lastSearchFactor = searchFactor;

        Loc moveLoc = bot->genMoveSynchronous(pla,tc,searchFactor);
        bool isLegal = bot->isLegal(moveLoc,pla);
        if(moveLoc == Board::NULL_LOC || !isLegal) {
          responseIsError = true;
          response = "genmove returned null location or illegal move";
          ostringstream sout;
          sout << "genmove null location or illegal move!?!" << "\n";
          sout << bot->getRootBoard() << "\n";
          sout << "Pla: " << playerToString(pla) << "\n";
          sout << "MoveLoc: " << Location::toString(moveLoc,bot->getRootBoard()) << "\n";
          logger.write(sout.str());
        }

        //Implement cleanupBeforePass hack - the bot wants to pass, so instead cleanup if there is something to clean
        if(cleanupBeforePass && moveLoc == Board::PASS_LOC) {
          Board board = bot->getRootBoard();
          BoardHistory hist = bot->getRootHist();
          Color* safeArea = bot->getSearch()->rootSafeArea;
          assert(safeArea != NULL);
          //Scan the board for any spot that is adjacent to an opponent group that is part of our pass-alive territory.
          for(int y = 0; y<board.y_size; y++) {
            for(int x = 0; x<board.x_size; x++) {
              Loc otherLoc = Location::getLoc(x,y,board.x_size);
              if(moveLoc == Board::PASS_LOC &&
                 board.colors[otherLoc] == C_EMPTY &&
                 safeArea[otherLoc] == pla &&
                 board.isAdjacentToPla(otherLoc,getOpp(pla)) &&
                 hist.isLegal(board,otherLoc,pla)
              ) {
                moveLoc = otherLoc;
              }
            }
          }
        }


        double winLossValue;
        double expectedScore;
        {
          ReportedSearchValues values = bot->getSearch()->getRootValuesAssertSuccess();
          winLossValue = values.winLossValue;
          expectedScore = values.expectedScore;
        }

        recentWinLossValues.push_back(winLossValue);

        bool resigned = false;
        if(allowResignation) {
          const BoardHistory hist = bot->getRootHist();
          const Board initialBoard = hist.initialBoard;

          //Assume an advantage of 15 * number of black stones beyond the one black normally gets on the first move and komi
          int extraBlackStones = numHandicapStones(hist);
          if(hist.initialPla == P_WHITE && extraBlackStones > 0)
            extraBlackStones -= 1;
          double handicapBlackAdvantage = 15.0 * extraBlackStones + (7.5 - hist.rules.komi);

          int minTurnForResignation = 0;
          double noResignationWhenWhiteScoreAbove = initialBoard.x_size * initialBoard.y_size;
          if(handicapBlackAdvantage > 2.0 && pla == P_WHITE) {
            //Play at least some moves no matter what
            minTurnForResignation = 1 + initialBoard.x_size * initialBoard.y_size / 6;

            //In a handicap game, also only resign if the expected score difference is well behind schedule assuming
            //that we're supposed to catch up over many moves.
            double numTurnsToCatchUp = 0.60 * initialBoard.x_size * initialBoard.y_size - minTurnForResignation;
            double numTurnsSpent = (double)(hist.moveHistory.size()) - minTurnForResignation;
            if(numTurnsToCatchUp <= 1.0)
              numTurnsToCatchUp = 1.0;
            if(numTurnsSpent <= 0.0)
              numTurnsSpent = 0.0;
            if(numTurnsSpent > numTurnsToCatchUp)
              numTurnsSpent = numTurnsToCatchUp;

            double resignScore = -handicapBlackAdvantage * ((numTurnsToCatchUp - numTurnsSpent) / numTurnsToCatchUp);
            resignScore -= 5.0; //Always require at least a 5 point buffer
            resignScore -= handicapBlackAdvantage * 0.15; //And also require a 15% of the initial handicap

            noResignationWhenWhiteScoreAbove = resignScore;
          }

          Player resignPlayerThisTurn = C_EMPTY;
          if(winLossValue < resignThreshold)
            resignPlayerThisTurn = P_WHITE;
          else if(winLossValue > -resignThreshold)
            resignPlayerThisTurn = P_BLACK;

          if(resignPlayerThisTurn == pla &&
             bot->getRootHist().moveHistory.size() >= minTurnForResignation &&
             !(pla == P_WHITE && expectedScore > noResignationWhenWhiteScoreAbove))
            resigned = true;
        }

        if(resigned)
          response = "resign";
        else
          response = Location::toString(moveLoc,bot->getRootBoard());

        if(logSearchInfo) {
          Search* search = bot->getSearch();
          ostringstream sout;
          Board::printBoard(sout, bot->getRootBoard(), moveLoc, &(bot->getRootHist().moveHistory));
          sout << bot->getRootHist().rules << "\n";
          sout << "Time taken: " << timer.getSeconds() << "\n";
          sout << "Root visits: " << search->numRootVisits() << "\n";
          sout << "NN rows: " << nnEval->numRowsProcessed() << endl;
          sout << "NN batches: " << nnEval->numBatchesProcessed() << endl;
          sout << "NN avg batch size: " << nnEval->averageProcessedBatchSize() << endl;
          sout << "PV: ";
          search->printPV(sout, search->rootNode, 25);
          sout << "\n";
          sout << "Tree:\n";
          search->printTree(sout, search->rootNode, PrintTreeOptions().maxDepth(1).maxChildrenToShow(10));
          logger.write(sout.str());
        }

        if(!resigned && moveLoc != Board::NULL_LOC && isLegal) {
          bool suc = bot->makeMove(moveLoc,pla);
          assert(suc);
          (void)suc; //Avoid warning when asserts are off
          maybeStartPondering = true;
        }

      }
    }

    else if(command == "showboard") {
      ostringstream sout;
      Board::printBoard(sout, bot->getRootBoard(), Board::NULL_LOC, &(bot->getRootHist().moveHistory));
      response = Global::trim(sout.str());
    }

    else if(command == "place_free_handicap") {
      int n;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for place_free_handicap but got '" + Global::concat(pieces," ") + "'";
      }
      else if(!Global::tryStringToInt(pieces[0],n)) {
        responseIsError = true;
        response = "Could not parse number of handicap stones: '" + pieces[0] + "'";
      }
      else if(n < 2) {
        responseIsError = true;
        response = "Number of handicap stones less than 2: '" + pieces[0] + "'";
      }
      else if(!bot->getRootBoard().isEmpty()) {
        responseIsError = true;
        response = "Board is not empty";
      }
      else {
        //If asked to place more, we just go ahead and only place up to 30, or a quarter of the board
        int xSize = bot->getRootBoard().x_size;
        int ySize = bot->getRootBoard().y_size;
        int maxHandicap = xSize*ySize / 4;
        if(maxHandicap > 30)
          maxHandicap = 30;
        if(n > maxHandicap)
          n = maxHandicap;

        Board board(xSize,ySize);
        Player pla = P_BLACK;
        BoardHistory hist(board,pla,bot->getRootHist().rules,0);
        double extraBlackTemperature = 0.25;
        bool adjustKomi = false;
        int numVisitsForKomi = 0;
        Rand rand;
        ExtraBlackAndKomi extraBlackAndKomi(n,hist.rules.komi,hist.rules.komi);
        Play::playExtraBlack(bot->getSearch(), logger, extraBlackAndKomi, board, hist, extraBlackTemperature, rand, adjustKomi, numVisitsForKomi);

        response = "";
        for(int y = 0; y<board.y_size; y++) {
          for(int x = 0; x<board.x_size; x++) {
            Loc loc = Location::getLoc(x,y,board.x_size);
            if(board.colors[loc] != C_EMPTY) {
              response += " " + Location::toString(loc,board);
            }
          }
        }
        response = Global::trim(response);

        bot->setPosition(pla,board,hist);
        updateKomiIfNew();
      }
    }

    else if(command == "set_free_handicap") {
      if(!bot->getRootBoard().isEmpty()) {
        responseIsError = true;
        response = "Board is not empty";
      }
      else {
        vector<Loc> locs;
        int xSize = bot->getRootBoard().x_size;
        int ySize = bot->getRootBoard().y_size;
        Board board(xSize,ySize);
        for(int i = 0; i<pieces.size(); i++) {
          Loc loc;
          bool suc = tryParseLoc(pieces[i],board,loc);
          if(!suc || loc == Board::PASS_LOC) {
            responseIsError = true;
            response = "Invalid handicap location: " + pieces[i];
          }
          locs.push_back(loc);
        }
        for(int i = 0; i<locs.size(); i++)
          board.setStone(locs[i],P_BLACK);
        Player pla = P_BLACK;
        BoardHistory hist(board,pla,bot->getRootHist().rules,0);

        bot->setPosition(pla,board,hist);
        updateKomiIfNew();
      }
    }

    else if(command == "final_score") {
      //Returns the resulting score if this position were scored AS-IS (players repeatedly passing until the game ends),
      //rather than attempting to estimate what the score would be with further playouts
      Board board = bot->getRootBoard();
      BoardHistory hist = bot->getRootHist();

      //For GTP purposes, we treat noResult as a draw since there is no provision for anything else.
      if(!hist.isGameFinished)
        hist.endAndScoreGameNow(board);

      if(hist.winner == C_EMPTY)
        response = "0";
      else if(hist.winner == C_BLACK)
        response = "B+" + Global::strprintf("%.1f",-hist.finalWhiteMinusBlackScore);
      else if(hist.winner == C_WHITE)
        response = "W+" + Global::strprintf("%.1f",hist.finalWhiteMinusBlackScore);
      else
        ASSERT_UNREACHABLE;
    }

    else if(command == "final_status_list") {
      int statusMode = 0;
      if(pieces.size() != 1) {
        responseIsError = true;
        response = "Expected one argument for final_status_list but got '" + Global::concat(pieces," ") + "'";
      }
      else {
        if(pieces[0] == "alive")
          statusMode = 0;
        else if(pieces[0] == "seki")
          statusMode = 1;
        else if(pieces[0] == "dead")
          statusMode = 2;
        else {
          responseIsError = true;
          response = "Argument to final_status_list must be 'alive' or 'seki' or 'dead'";
          statusMode = 3;
        }

        if(statusMode < 3) {
          vector<Loc> locsToReport;
          Board board = bot->getRootBoard();
          BoardHistory hist = bot->getRootHist();

          if(hist.isGameFinished && hist.isNoResult) {
            //Treat all stones as alive under a no result
            if(statusMode == 0) {
              for(int y = 0; y<board.y_size; y++) {
                for(int x = 0; x<board.x_size; x++) {
                  Loc loc = Location::getLoc(x,y,board.x_size);
                  if(board.colors[loc] != C_EMPTY)
                    locsToReport.push_back(loc);
                }
              }
            }
          }
          else {
            Color area[Board::MAX_ARR_SIZE];
            hist.endAndScoreGameNow(board,area);
            for(int y = 0; y<board.y_size; y++) {
              for(int x = 0; x<board.x_size; x++) {
                Loc loc = Location::getLoc(x,y,board.x_size);
                if(board.colors[loc] != C_EMPTY) {
                  if(statusMode == 0 && board.colors[loc] == area[loc])
                    locsToReport.push_back(loc);
                  else if(statusMode == 2 && board.colors[loc] != area[loc])
                    locsToReport.push_back(loc);
                }
              }
            }
          }

          response = "";
          for(int i = 0; i<locsToReport.size(); i++) {
            Loc loc = locsToReport[i];
            if(i > 0)
              response += " ";
            response += Location::toString(loc,board);
          }
        }
      }
    }

    else if(command == "lz-analyze" || command == "kata-analyze") {
      int numArgsParsed = 0;

      Player pla = bot->getRootPla();
      double lzAnalyzeInterval = 1e30;
      int minMoves = 0;
      bool parseFailed = false;

      if(pieces.size() > numArgsParsed && tryParsePlayer(pieces[numArgsParsed],pla))
        numArgsParsed += 1;

      if(pieces.size() > numArgsParsed &&
         Global::tryStringToDouble(pieces[numArgsParsed],lzAnalyzeInterval) &&
         !isnan(lzAnalyzeInterval) && lzAnalyzeInterval >= 0 && lzAnalyzeInterval < 1e20)
        numArgsParsed += 1;

      while(pieces.size() > numArgsParsed) {
        if(pieces[numArgsParsed] == "interval") {
          numArgsParsed += 1;
          if(pieces.size() > numArgsParsed &&
             Global::tryStringToDouble(pieces[numArgsParsed],lzAnalyzeInterval) &&
             !isnan(lzAnalyzeInterval) && lzAnalyzeInterval >= 0 && lzAnalyzeInterval < 1e20) {
            numArgsParsed += 1;
            continue;
          }
          else {
            parseFailed = true;
            break;
          }
        }

        //Parse it but ignore it since we don't support excluding moves right now
        else if(pieces[numArgsParsed] == "avoid" || pieces[numArgsParsed] == "allow") {
          numArgsParsed += 1;
          for(int r = 0; r<3; r++) {
            if(pieces.size() > numArgsParsed)
              numArgsParsed += 1;
            else
              parseFailed = true;
          }
          if(parseFailed)
            break;
          continue;
        }

        else if(pieces[numArgsParsed] == "minmoves") {
          numArgsParsed += 1;
          if(pieces.size() > numArgsParsed &&
             Global::tryStringToInt(pieces[numArgsParsed],minMoves) &&
             minMoves >= 0 && minMoves < 1000000000) {
            numArgsParsed += 1;
            continue;
          }
          else {
            parseFailed = true;
            break;
          }
        }

        parseFailed = true;
        break;
      }

      if(parseFailed) {
        responseIsError = true;
        response = "Could not parse lz-analyze arguments or arguments out of range: '" + Global::concat(pieces," ") + "'";
      }
      else {
        lzAnalyzeInterval = lzAnalyzeInterval * 0.01; //Convert from centiseconds to seconds

        static const int analysisPVLen = 9;
        std::function<void(Search* search)> callback;
        if(command == "lz-analyze") {
          callback = [minMoves,pla](Search* search) {
            vector<AnalysisData> buf;
            search->getAnalysisData(buf,minMoves,false,analysisPVLen);
            if(buf.size() <= 0)
              return;

            const Board board = search->getRootBoard();
            for(int i = 0; i<buf.size(); i++) {
              if(i > 0)
                cout << " ";
              const AnalysisData& data = buf[i];
              double winrate = 0.5 * (1.0 + data.winLossValue);
              winrate = pla == P_BLACK ? -winrate : winrate;
              cout << "info";
              cout << " move " << Location::toString(data.move,board);
              cout << " visits " << data.numVisits;
              cout << " winrate " << round(winrate * 10000.0);
              cout << " prior " << round(data.policyPrior * 10000.0);
              cout << " order " << data.order;
              cout << " pv";
              for(int j = 0; j<data.pv.size(); j++)
                cout << " " << Location::toString(data.pv[j],board);
            }
            cout << endl;
          };
        }
        else if(command == "kata-analyze") {
          callback = [minMoves,pla](Search* search) {
            vector<AnalysisData> buf;
            search->getAnalysisData(buf,minMoves,false,analysisPVLen);
            if(buf.size() <= 0)
              return;

            const Board board = search->getRootBoard();
            for(int i = 0; i<buf.size(); i++) {
              if(i > 0)
                cout << " ";
              const AnalysisData& data = buf[i];
              double winrate = 0.5 * (1.0 + data.winLossValue);
              winrate = pla == P_BLACK ? -winrate : winrate;
              cout << "info";
              cout << " move " << Location::toString(data.move,board);
              cout << " visits " << data.numVisits;
              cout << " utility " << data.utility;
              cout << " winrate " << winrate;
              cout << " scoreMean " << data.scoreMean;
              cout << " scoreStdev " << data.scoreStdev;
              cout << " prior " << data.policyPrior;
              cout << " order " << data.order;
              cout << " pv";
              for(int j = 0; j<data.pv.size(); j++)
                cout << " " << Location::toString(data.pv[j],board);
            }
            cout << endl;
          };

        }
        else
          ASSERT_UNREACHABLE;

        double searchFactor = 1e40; //go basically forever
        bot->analyze(pla, searchFactor, lzAnalyzeInterval, callback);
        currentlyAnalyzing = true;
      }
    }

    else if(command == "stop") {
      //Stop any ongoing ponder or analysis
      bot->stopAndWait();
    }

    else {
      responseIsError = true;
      response = "unknown command";
    }


    //Postprocessing of response
    if(hasId)
      response = Global::intToString(id) + " " + response;
    else
      response = " " + response;

    if(responseIsError)
      response = "?" + response;
    else
      response = "=" + response;

    cout << response << endl;

    //GTP needs extra newline, except if currently analyzing, defer the newline until we actually stop analysis
    if(!currentlyAnalyzing)
      cout << endl;

    if(logAllGTPCommunication)
      logger.write(response);

    if(shouldQuitAfterResponse)
      break;

    if(maybeStartPondering && ponderingEnabled)
      bot->ponder(lastSearchFactor);

  } //Close read loop


  delete bot;
  delete nnEval;
  NeuralNet::globalCleanup();

  logger.write("All cleaned up, quitting");
  return 0;
}

