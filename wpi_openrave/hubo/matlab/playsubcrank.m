%this script is meant to play back the trajectories generated by testsubcrank
clear all;
close all;
orEnvLoadScene('',1)
robotid = orEnvCreateRobot('HRP3','models/hrp3/hrp3d_rightleg.robot.xml');
orEnvCreateKinBody('subroom','models/subroom.kinbody.xml');
crankid = orEnvCreateRobot('crank','models/crank.robot.xml');
xoffset = 0.07;
orBodySetTransform(robotid,[0.9969    0.0783         0   -0.0783    0.9969         0         0         0    1.0000      -1.5602+xoffset    2.5666    0.1545]);
orBodySetTransform(crankid,[reshape(rodrigues([0 pi/2 0]),1,9)';[-0.9566+xoffset    2.7186    1.2225]'])

pausetime = 0.2;
orEnvSetOptions('debug 3')
orEnvSetOptions('publishanytime 0');

probs.cbirrt = orEnvCreateProblem('CBiRRT','HRP3');
probs.crankmover = orEnvCreateProblem('CBiRRT','crank',0);
manips = orRobotGetManipulators(robotid);

jointdofs = 0:41;
activedofs = setdiff(jointdofs,[manips{1}.armjoints,manips{4}.joints,manips{5}.joints]);

dofvals = [0.1804;-0.4307;0.9943;-0.5118;-0.1802;0.0093;-0.0093;0.1801;-0.5129;0.9965;-0.4319;-0.1804;0;0;0;0;0;0;0;-0.8;0;0;0;0;0;0;0;0.4;0.5;0;0;0;-0.8;0;0;0;0;0;0;0;-0.4;-0.5;];
orProblemSendCommand('SetCamView -0.00328895 0.00649324 0.768259 -0.640098 -1.30788 4.99505 1.22135',probs.cbirrt)





orRobotSetDOFValues(robotid,dofvals,jointdofs);
pause(pausetime)

%start recording here
orProblemSendCommand(['traj movetraj0.txt'],probs.cbirrt);
orEnvWait(robotid);
pause(pausetime)
orProblemSendCommand(['traj movetraj1.txt'],probs.cbirrt);
orProblemSendCommand(['traj movetraj1.txt'],probs.crankmover)
orEnvWait(robotid);
pause(pausetime)


for i = 1:4
    orProblemSendCommand(['traj movetraj2.txt'],probs.cbirrt);
    orEnvWait(robotid);
    pause(pausetime)
    orProblemSendCommand(['traj movetraj3.txt'],probs.cbirrt);
    orProblemSendCommand(['traj movetraj3.txt'],probs.crankmover);
    orEnvWait(robotid);
    pause(pausetime)
end


orProblemSendCommand(['traj movetraj4.txt'],probs.cbirrt);
orEnvWait(robotid);
pause(pausetime)